/*
   Copyright (c) 2019-2021 ZettaDB inc. All rights reserved.

   This source code is licensed under Apache 2.0 License,
   combined with Common Clause Condition 1.0, as detailed in the NOTICE file.
*/

#include "sys_config.h"
#include "global.h"
#include "sys.h"
#include "log.h"
#include "job.h"
#include "config.h"
#include "shard.h"
#include "thread_manager.h"
#include "http_server.h"
#include "http_client.h"
#include "hdfs_client.h"
#include <utility>

System *System::m_global_instance = NULL;
extern std::string log_file_path;
extern int64_t storage_instance_port_start;
extern int64_t computer_instance_port_start;

System::~System()
{
	Http_server::get_instance()->do_exit = 1;
	Http_server::get_instance()->join_all();
	delete Http_server::get_instance();

	Job::get_instance()->do_exit = 1;
	Job::get_instance()->join_all();
	delete Job::get_instance();

	//delete Hdfs_client::get_instance();
	delete Http_client::get_instance();
	delete Machine_info::get_instance();

	for (auto &i:kl_clusters)
		delete i;

	delete Configs::get_instance();
	delete Logger::get_instance();

	Thread_manager::do_exit = 1;
	Thread_manager::get_instance()->join_all();
	pthread_mutex_destroy(&mtx);
	pthread_mutexattr_destroy(&mtx_attr);
}

/*
  We assume the metadata cluster nodes in sn's pg_cluster_meta_nodes are always
  up to date, and we allow the metadata shard master connection configs to be
  obsolete as long as it's still in the metadata shard and all metadata shard
  nodes it has in pg_cluster_meta_nodes belong to this shard, although some of
  them could be out of the GR cluster currently.
  @retval: -1: invalid MGR topology; 
*/
int System::setup_metadata_shard()
{
	Scopped_mutex sm(mtx);
	int ret = 0;
	bool is_master = false;
	int nrows = 0, master_port = 0;
	std::string master_ip;
	Shard_node *sn = meta_shard.get_master();
	if (!sn)
	{
		sn = meta_shard.get_node_by_ip_port(meta_svr_ip, meta_svr_port);
	}
	else
	{
		is_master = true;
		sn->get_ip_port(master_ip, master_port);
		if (meta_shard.set_master(sn))
			syslog(Logger::INFO,
		   		"Found primary node of shard(%s.%s, %u) to be (%s:%d, %u)",
		   		meta_shard.get_cluster_name().c_str(),
				meta_shard.get_name().c_str(),
		   		meta_shard.get_id(), master_ip.c_str(), master_port,
				sn->get_id());
	}

	if (!sn)
	{
		sn = new Shard_node(0,
				&meta_shard, meta_svr_ip.c_str(), meta_svr_port,
				meta_svr_user.c_str(), meta_svr_pwd.c_str());

		meta_shard.add_node(sn);
	}

	ret = sn->send_stmt(SQLCOM_SELECT, CONST_STR_PTR_LEN(
		"select MEMBER_HOST, MEMBER_PORT from performance_schema.replication_group_members where MEMBER_ROLE = 'PRIMARY' and MEMBER_STATE = 'ONLINE'"), 3);
	if (ret)
		return ret;

	MYSQL_RES *result = sn->get_result();
	MYSQL_ROW row;
	char *endptr = NULL;
	Shard_node *master_sn = NULL;

	while ((row = mysql_fetch_row(result)))
	{
		unsigned long *lengths = mysql_fetch_lengths(result);
		int port = strtol(row[1], &endptr, 10);
		Assert(endptr == NULL || *endptr == '\0');

		/*
		  meta_svr_ip:meta_svr_port is the current master node,
		*/
		if (strncmp(row[0], meta_svr_ip.c_str(),
			std::min(meta_svr_ip.length(), lengths[0])) == 0 &&
			port == meta_svr_port)
		{
			is_master = true;
			master_sn = sn;
			master_ip = meta_svr_ip;
			master_port = meta_svr_port;
			meta_shard.set_master(master_sn);
		}
		else
		{
			master_ip = row[0];
			master_port = strtol(row[1], &endptr, 10);
			Assert(endptr == NULL || *endptr == '\0');
		}

		nrows++;
		if (nrows > 1)
		{
			syslog(Logger::ERROR, "Multiple(%d) primary nodes found: %s:%d.",
				   row[0], port);
		}
	}
	sn->free_mysql_result();

	if (nrows > 1)
		return -1;

	if (nrows == 0)
	{
		/*
		  This node is out of the meta shard, and we don't know any other node
		  of the meta shard, so suppose it is the master, and
		  connect to it to get the list of metadata shard nodes. If it contains
		  nodes not in current meta shard, kunlun DDC won't be able
		  to use the latest meta data.
		*/
		is_master = true;
		sn->get_ip_port(master_ip, master_port);
		if (meta_shard.set_master(sn))
		{
			ret = sn->send_stmt(SQLCOM_SELECT, CONST_STR_PTR_LEN("select count(*) from meta_db_nodes"), 3);
			if (ret)
				return ret;

			result = sn->get_result();
			if ((row = mysql_fetch_row(result)))
			{
				if(row[0] != NULL && strcmp(row[0],"1") == 0)
				{
					meta_shard.set_mode(Shard::HAVL_mode::HA_no_rep);
					syslog(Logger::INFO, "set meta shard as HA_no_rep");
				}
			}
			sn->free_mysql_result();
			
			syslog(Logger::WARNING,
		   		"Suppose primary node of shard(%s.%s, %u) to be (%s:%d, %u) since it's out of the meta-shard MGR cluster. It must have latest list of metadata nodes otherwise Kunlun DDC won't be able to work correctly.",
		   		meta_shard.get_cluster_name().c_str(),
				meta_shard.get_name().c_str(),
		   		meta_shard.get_id(), master_ip.c_str(), master_port,
				sn->get_id());
		}
	}

	/*
	  If sn is already the current master node, fetch all meta shard node from it;
	  otherwise fetch only the current master's user/pwd from it and then connect
	  to the current master to fetch all other meta shard nodes.
	*/
	meta_shard.fetch_meta_shard_nodes(sn, is_master,
		master_ip.c_str(), master_port);
	if (!is_master)
	{
		ret = meta_shard.fetch_meta_shard_nodes(sn, false,
			master_ip.c_str(), master_port);
	}

	return ret;
}

/*
  Connect to meta data master node, get all shards' configs, and update each node.
  If a node isn't in the query result, remove it.
  If a node doesn't exist, create it and add it to its shard.
*/
int System::refresh_shards_from_metadata_server()
{
	Scopped_mutex sm(mtx);
	return meta_shard.refresh_shards(kl_clusters);
}

/*
  Connect to meta data master node, get all computer' configs, and update each node.
  If a node isn't in the query result, remove it.
  If a node doesn't exist, create it and add it to its computers.
*/
int System::refresh_computers_from_metadata_server()
{
	Scopped_mutex sm(mtx);
	return meta_shard.refresh_computers(kl_clusters);
}

/*
  Connect to storage node, get tables's rows & pages, and update to computer nodes.
*/
int System::refresh_storages_info_to_computers()
{
	Scopped_mutex sm(mtx);
	for (auto &cluster:kl_clusters)
		cluster->refresh_storages_to_computers();
	return 0;
}

/*
  Connect to storage node, get num_tablets & space_volumn, and update to computer nodes and meta shard.
*/
int System::refresh_storages_info_to_computers_metashard()
{
	Scopped_mutex sm(mtx);
	for (auto &cluster:kl_clusters)
		cluster->refresh_storages_to_computers_metashard(meta_shard);
	return 0;
}

/*
  Connect to meta data master node, truncate unused commit log partitions
*/
int System::truncate_commit_log_from_metadata_server()
{
	Scopped_mutex sm(mtx);
	kl_clusters[0]->truncate_commit_log_from_metadata_server(kl_clusters, meta_shard);
	return 0;
}

/*
  Read config file and initialize config settings;
  Connect to metadata shard and get the storage shards to work on, set up
  Shard objects and the connections to each shard node.
*/
int System::create_instance(const std::string&cfg_path)
{
	m_global_instance = new System(cfg_path);
	Configs *cfg = Configs::get_instance();
	int ret = 0;

	if ((ret = Logger::create_instance()))
		goto end;
	if ((ret = cfg->process_config_file(cfg_path)))
		goto end;
	if ((ret = Logger::get_instance()->init(log_file_path)) != 0)
		goto end;
	if ((ret = (Thread_manager::get_instance()==NULL)) != 0)
		goto end;
	if ((ret = (Machine_info::get_instance()==NULL)) != 0)
		goto end;
	//if ((ret = (Hdfs_client::get_instance()==NULL)) != 0)
	//	goto end;
	if ((ret = (Http_client::get_instance()==NULL)) != 0)
		goto end;
	if ((ret = Job::get_instance()->start_job_thread()) != 0)
		goto end;
	if ((ret = Http_server::get_instance()->start_http_thread()) != 0)
		goto end;
	
	if (m_global_instance->setup_metadata_shard() != 0)
	{
		syslog(Logger::ERROR, "setup_metadata_shard fail");
	}

end:
	return ret;
}

/*
  Find a proper shard for worker thread 'thd' to work on.
  return true if one is found and associated with 'thd', false otherwise.

  This method should be called in the main thread, only by which the storage_shards
  is modified, so no need for a mtx lock.
*/
bool System::acquire_shard(Thread *thd, bool force)
{
	for (auto &cluster:kl_clusters)
		for (auto &sd:cluster->storage_shards)
		{
			if (sd->set_thread_handler(thd, force))
				return true;
		}

	return false;
}

// the next several function for auto cluster operation 
int System::execute_metadate_opertation(enum_sql_command command, const std::string & str_sql)
{
	Scopped_mutex sm(mtx);

	return meta_shard.execute_metadate_opertation(SQLCOM_INSERT, str_sql);
}

int System::get_comp_nodes_id_seq(int &comps_id)
{
	Scopped_mutex sm(mtx);

	return meta_shard.get_comp_nodes_id_seq(comps_id);
}

int System::get_max_cluster_id(int &cluster_id)
{
	Scopped_mutex sm(mtx);

	return meta_shard.get_max_cluster_id(cluster_id);
}

int System::get_max_shard_name_id(std::string &cluster_name, int &shard_id)
{
	Scopped_mutex sm(mtx);

	//storage shard
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//compare every shard name id
		for (auto &shard:cluster->storage_shards)
		{
			char *p = (char*)(shard->get_name().c_str());
			if(p==NULL)
				continue;

			//skip no digit
			while(*p < '0' || *p > '9')
				p++;

			int i = atoi(p);
			if(shard_id<i)
				shard_id = i;
		}

		break;
	}

	return 0;
}

int System::get_max_comp_name_id(std::string &cluster_name, int &comp_id)
{
	Scopped_mutex sm(mtx);

	//storage shard
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//compare every shard name id
		for (auto &comp:cluster->computer_nodes)
		{
			char *p = (char*)(comp->get_name().c_str());
			if(p==NULL)
				continue;

			//skip no digit
			while(*p < '0' || *p > '9')
				p++;

			int i = atoi(p);
			if(comp_id<i)
				comp_id = i;
		}

		break;
	}

	return 0;
}

int System::get_server_nodes_from_metadata(std::vector<Machine*> &vec_machines)
{
	Scopped_mutex sm(mtx);

	return meta_shard.get_server_nodes_from_metadata(vec_machines);
}

int System::get_backup_info_from_metadata(std::string &cluster_name, std::string &timestamp, Tpye_cluster_info &cluster_info)
{
	Scopped_mutex sm(mtx);

	return meta_shard.get_backup_info_from_metadata(cluster_name, timestamp, cluster_info);
}

bool System::check_machine_hostaddr(std::string &hostaddr)
{
	Scopped_mutex sm(mtx);

	return meta_shard.check_machine_hostaddr(hostaddr);
}

bool System::check_cluster_name(std::string &cluster_name)
{
	Scopped_mutex sm(mtx);

	for (auto &cluster:kl_clusters)
	{
		if(cluster->get_name() == cluster_name)
			return true;
	}

	return false;
}

bool System::check_cluster_shard_name(std::string &cluster_name, std::string &shard_name)
{
	Scopped_mutex sm(mtx);

	//storage shard
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//compare to every shard name
		for (auto &shard:cluster->storage_shards)
		{
			if(shard_name == shard->get_name())
				return true;
		}
			
		break;
	}
	return false;
}

bool System::check_cluster_shard_more(std::string &cluster_name)
{
	Scopped_mutex sm(mtx);

	//storage shard
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		return (cluster->storage_shards.size()>1);
	}
	return false;
}

bool System::check_cluster_shard_node_more(std::string &cluster_name, std::string &shard_name)
{
	Scopped_mutex sm(mtx);

	//storage shard
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//compare to every shard name
		for (auto &shard:cluster->storage_shards)
		{
			if(shard_name == shard->get_name())
				return (shard->get_nodes().size()>1);
		}
			
		break;
	}
	return false;
}

bool System::check_cluster_comp_more(std::string &cluster_name)
{
	Scopped_mutex sm(mtx);

	//comps
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		return (cluster->computer_nodes.size()>1);
	}
	return false;
}

bool System::get_cluster_shard_name(std::string &cluster_name, std::vector<std::string> &vec_shard_name)
{
	Scopped_mutex sm(mtx);

	//storage shard
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//get every shard name
		for (auto &shard:cluster->storage_shards)
		{
			vec_shard_name.emplace_back(shard->get_name());
		}

		break;
	}
	return (vec_shard_name.size() > 0);
}

bool System::get_cluster_shard_order(std::string &cluster_name, std::string &shard_name, int &order)
{
	Scopped_mutex sm(mtx);

	//storage shard
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//sort shard by id
		sort(cluster->storage_shards.begin(), cluster->storage_shards.end(), 
				[](Shard * a, Shard * b){return a->get_id() < a->get_id();});

		for(order=0; order<cluster->storage_shards.size(); order++)
		{
			if(shard_name == cluster->storage_shards[order]->get_name())
				break;
		}

		break;
	}
	
	return true;
}

bool System::get_meta_info(std::vector<Tpye_Ip_Port_User_Pwd> &meta)
{
	Scopped_mutex sm(mtx);
	
	for(auto &node: meta_shard.get_nodes())
	{
		std::string ip,user,pwd;
		int port;
		
		node->get_ip_port(ip, port);
		node->get_user_pwd(user, pwd);

		meta.emplace_back(std::make_tuple(ip, port, user, pwd));
	}

	return (meta.size() > 0);
}

bool System::get_machine_instance_port(Machine* machine)
{
	Scopped_mutex sm(mtx);

	machine->instances = 0;
	machine->instance_storage = 0;
	machine->instance_computer = 0;
	machine->port_storage = 0;
	machine->port_computer = 0;

	meta_shard.get_meta_instance(machine);
	meta_shard.get_storage_instance_port(machine);
	meta_shard.get_computer_instance_port(machine);

	machine->instances = machine->instance_storage + machine->instance_computer;

	if(machine->port_storage < storage_instance_port_start)
		machine->port_storage = storage_instance_port_start;
	else
		machine->port_storage += 3;

	if(machine->port_computer < computer_instance_port_start)
		machine->port_computer = computer_instance_port_start;
	else
		machine->port_computer += 1;

	return true;
}

bool System::update_instance_status(Tpye_Ip_Port &ip_port, std::string &status, int &type)
{
	Scopped_mutex sm(mtx);

	if(meta_shard.update_instance_status(ip_port, status, type))
	{
		//syslog(Logger::ERROR, "update_instance_status error");
		return false;
	}

	return true;
}

bool System::get_node_instance(cJSON *root, std::string &str_ret)
{
	Scopped_mutex sm(mtx);
	
	bool ret = false;
	int node_count = 0;
	cJSON *item;

	std::vector<std::string> vec_node_ip;
	int ip_count = 0;
	while(true)
	{
		std::string node_ip = "node_ip" + std::to_string(ip_count++);
		item = cJSON_GetObjectItem(root, node_ip.c_str());
		if(item == NULL)
			break;

		vec_node_ip.emplace_back(item->valuestring);
	}
	
	item = cJSON_GetObjectItem(root, "instance_type");
	if(item == NULL)
	{
		cJSON *ret_root;
		cJSON *ret_item;
		char *ret_cjson;
		ret_root = cJSON_CreateObject();

		//meta node
		node_count = 0;
		for(auto &node: meta_shard.get_nodes())
		{
			std::string ip;
			int port;

			node->get_ip_port(ip, port);

			bool ip_match = (vec_node_ip.size() == 0);
			for(auto &node_ip: vec_node_ip)
			{
				if(ip == node_ip)
				{
					ip_match = true;
					break;
				}
			}

			if(!ip_match)
				continue;

			std::string user,pwd;
			node->get_user_pwd(user, pwd);

			std::string str;
			ret_item = cJSON_CreateObject();
			str = "meta_instance" + std::to_string(node_count++);
			cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
			
			cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
			cJSON_AddNumberToObject(ret_item, "port", port);
			cJSON_AddStringToObject(ret_item, "user", user.c_str());
			cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
		}

		//storage node
		node_count = 0;
		for (auto &cluster:kl_clusters)
			for (auto &shard:cluster->storage_shards)
				for (auto &node:shard->get_nodes())
		{
			std::string ip;
			int port;

			node->get_ip_port(ip, port);

			bool ip_match = (vec_node_ip.size() == 0);
			for(auto &node_ip: vec_node_ip)
			{
				if(ip == node_ip)
				{
					ip_match = true;
					break;
				}
			}

			if(!ip_match)
				continue;

			std::string user,pwd;
			node->get_user_pwd(user, pwd);

			std::string str;
			ret_item = cJSON_CreateObject();
			str = "storage_instance" + std::to_string(node_count++);
			cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
			
			cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
			cJSON_AddNumberToObject(ret_item, "port", port);
			cJSON_AddStringToObject(ret_item, "user", user.c_str());
			cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
			cJSON_AddStringToObject(ret_item, "cluster", cluster->get_name().c_str());
			cJSON_AddStringToObject(ret_item, "shard", shard->get_name().c_str());
		}

		//computer node
		node_count = 0;
		for (auto &cluster:kl_clusters)
			for (auto &node:cluster->computer_nodes)
		{
			std::string ip;
			int port;
		
			node->get_ip_port(ip, port);
		
			bool ip_match = (vec_node_ip.size() == 0);
			for(auto &node_ip: vec_node_ip)
			{
				if(ip == node_ip)
				{
					ip_match = true;
					break;
				}
			}
		
			if(!ip_match)
				continue;
		
			std::string user,pwd;
			node->get_user_pwd(user, pwd);
		
			std::string str;
			ret_item = cJSON_CreateObject();
			str = "computer_instance" + std::to_string(node_count++);
			cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
			
			cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
			cJSON_AddNumberToObject(ret_item, "port", port);
			cJSON_AddStringToObject(ret_item, "user", user.c_str());
			cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
			cJSON_AddStringToObject(ret_item, "cluster", cluster->get_name().c_str());
			cJSON_AddStringToObject(ret_item, "comp", node->get_name().c_str());
		}

		ret_cjson = cJSON_Print(ret_root);
		str_ret = ret_cjson;

		if(ret_root != NULL)
			cJSON_Delete(ret_root);
		if(ret_cjson != NULL)
			free(ret_cjson);
	
		return true;
	}

	if(strcmp(item->valuestring, "meta_instance") == 0)
	{
		cJSON *ret_root;
		cJSON *ret_item;
		char *ret_cjson;
		ret_root = cJSON_CreateObject();
		
		for(auto &node: meta_shard.get_nodes())
		{
			std::string ip;
			int port;

			node->get_ip_port(ip, port);

			bool ip_match = (vec_node_ip.size() == 0);
			for(auto &node_ip: vec_node_ip)
			{
				if(ip == node_ip)
				{
					ip_match = true;
					break;
				}
			}

			if(!ip_match)
				continue;

			std::string user,pwd;
			node->get_user_pwd(user, pwd);

			std::string str;
			ret_item = cJSON_CreateObject();
			str = "meta_instance" + std::to_string(node_count++);
			cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
			
			cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
			cJSON_AddNumberToObject(ret_item, "port", port);
			cJSON_AddStringToObject(ret_item, "user", user.c_str());
			cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
		}

		ret_cjson = cJSON_Print(ret_root);
		str_ret = ret_cjson;

		if(ret_root != NULL)
			cJSON_Delete(ret_root);
		if(ret_cjson != NULL)
			free(ret_cjson);

		ret = true;
	}
	else if(strcmp(item->valuestring, "storage_instance") == 0)
	{
		cJSON *ret_root;
		cJSON *ret_item;
		char *ret_cjson;
		ret_root = cJSON_CreateObject();

		for (auto &cluster:kl_clusters)
			for (auto &shard:cluster->storage_shards)
				for (auto &node:shard->get_nodes())
		{
			std::string ip;
			int port;

			node->get_ip_port(ip, port);

			bool ip_match = (vec_node_ip.size() == 0);
			for(auto &node_ip: vec_node_ip)
			{
				if(ip == node_ip)
				{
					ip_match = true;
					break;
				}
			}

			if(!ip_match)
				continue;

			std::string user,pwd;
			node->get_user_pwd(user, pwd);

			std::string str;
			ret_item = cJSON_CreateObject();
			str = "storage_instance" + std::to_string(node_count++);
			cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
			
			cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
			cJSON_AddNumberToObject(ret_item, "port", port);
			cJSON_AddStringToObject(ret_item, "user", user.c_str());
			cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
			cJSON_AddStringToObject(ret_item, "cluster", cluster->get_name().c_str());
			cJSON_AddStringToObject(ret_item, "shard", shard->get_name().c_str());
		}

		ret_cjson = cJSON_Print(ret_root);
		str_ret = ret_cjson;

		if(ret_root != NULL)
			cJSON_Delete(ret_root);
		if(ret_cjson != NULL)
			free(ret_cjson);

		ret = true;
	}
	else if(strcmp(item->valuestring, "computer_instance") == 0)
	{
		cJSON *ret_root;
		cJSON *ret_item;
		char *ret_cjson;
		ret_root = cJSON_CreateObject();

		for (auto &cluster:kl_clusters)
			for (auto &node:cluster->computer_nodes)
		{
			std::string ip;
			int port;

			node->get_ip_port(ip, port);

			bool ip_match = (vec_node_ip.size() == 0);
			for(auto &node_ip: vec_node_ip)
			{
				if(ip == node_ip)
				{
					ip_match = true;
					break;
				}
			}

			if(!ip_match)
				continue;

			std::string user,pwd;
			node->get_user_pwd(user, pwd);

			std::string str;
			ret_item = cJSON_CreateObject();
			str = "computer_instance" + std::to_string(node_count++);
			cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
			
			cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
			cJSON_AddNumberToObject(ret_item, "port", port);
			cJSON_AddStringToObject(ret_item, "user", user.c_str());
			cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
			cJSON_AddStringToObject(ret_item, "cluster", cluster->get_name().c_str());
			cJSON_AddStringToObject(ret_item, "comp", node->get_name().c_str());
		}

		ret_cjson = cJSON_Print(ret_root);
		str_ret = ret_cjson;

		if(ret_root != NULL)
			cJSON_Delete(ret_root);
		if(ret_cjson != NULL)
			free(ret_cjson);

		ret = true;
	}
	
	return ret;
}

bool System::get_meta(cJSON *root, std::string &str_ret)
{
	Scopped_mutex sm(mtx);

	cJSON *ret_root;
	cJSON *ret_item;
	char *ret_cjson;
	int node_count=0;
	ret_root = cJSON_CreateObject();
	
	for(auto &node: meta_shard.get_nodes())
	{
		std::string ip,user,pwd;;
		int port;
		node->get_ip_port(ip, port);
		node->get_user_pwd(user, pwd);

		std::string str;
		ret_item = cJSON_CreateObject();
		str = "meta" + std::to_string(node_count++);
		cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
		
		cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
		cJSON_AddStringToObject(ret_item, "port", std::to_string(port).c_str());
		cJSON_AddStringToObject(ret_item, "user", user.c_str());
		cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
	}

	ret_cjson = cJSON_Print(ret_root);
	str_ret = ret_cjson;

	if(ret_root != NULL)
		cJSON_Delete(ret_root);
	if(ret_cjson != NULL)
		free(ret_cjson);

	return true;
}

bool System::get_cluster(cJSON *root, std::string &str_ret)
{
	Scopped_mutex sm(mtx);

	cJSON *ret_root;
	cJSON *ret_item;
	char *ret_cjson;
	int node_count=0;
	ret_root = cJSON_CreateObject();
	
	for (auto &cluster:kl_clusters)
	{
		std::string str;
		ret_item = cJSON_CreateObject();
		str = "cluster" + std::to_string(node_count++);
		cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
		
		cJSON_AddStringToObject(ret_item, "name", cluster->get_name().c_str());
		cJSON_AddStringToObject(ret_item, "shards", std::to_string(cluster->storage_shards.size()).c_str());
		cJSON_AddStringToObject(ret_item, "comps", std::to_string(cluster->computer_nodes.size()).c_str());
	}

	ret_cjson = cJSON_Print(ret_root);
	str_ret = ret_cjson;

	if(ret_root != NULL)
		cJSON_Delete(ret_root);
	if(ret_cjson != NULL)
		free(ret_cjson);

	return true;
}

bool System::get_storage(cJSON *root, std::string &str_ret)
{
	Scopped_mutex sm(mtx);

	cJSON *ret_root;
	cJSON *ret_item;
	cJSON *item;
	char *ret_cjson;
	int node_count=0;

	std::string cluster_name;
	item = cJSON_GetObjectItem(root, "cluster_name");
	if(item == NULL || item->valuestring == NULL)
	{
		syslog(Logger::ERROR, "get cluster_name error");
		return false;
	}
	cluster_name = item->valuestring;

	ret_root = cJSON_CreateObject();
	
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		for(auto &shard:cluster->storage_shards)
		{
			for(auto &node:shard->get_nodes())
			{
				std::string ip,user,pwd;
				int port;

				node->get_ip_port(ip, port);
				node->get_user_pwd(user, pwd);

				std::string str;
				ret_item = cJSON_CreateObject();
				str = "storage" + std::to_string(node_count++);
				cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
				
				cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
				cJSON_AddStringToObject(ret_item, "port", std::to_string(port).c_str());
				cJSON_AddStringToObject(ret_item, "user", user.c_str());
				cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
			}
		}

		break;
	}

	ret_cjson = cJSON_Print(ret_root);
	str_ret = ret_cjson;

	if(ret_root != NULL)
		cJSON_Delete(ret_root);
	if(ret_cjson != NULL)
		free(ret_cjson);

	return true;
}

bool System::get_computer(cJSON *root, std::string &str_ret)
{
	Scopped_mutex sm(mtx);

	cJSON *ret_root;
	cJSON *ret_item;
	cJSON *item;
	char *ret_cjson;
	int node_count=0;

	std::string cluster_name;
	item = cJSON_GetObjectItem(root, "cluster_name");
	if(item == NULL || item->valuestring == NULL)
	{
		syslog(Logger::ERROR, "get cluster_name error");
		return false;
	}
	cluster_name = item->valuestring;

	ret_root = cJSON_CreateObject();
	
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		for(auto &comp:cluster->computer_nodes)
		{
			std::string ip,user,pwd;
			int port;

			comp->get_ip_port(ip, port);
			comp->get_user_pwd(user, pwd);

			std::string str;
			ret_item = cJSON_CreateObject();
			str = "computer" + std::to_string(node_count++);
			cJSON_AddItemToObject(ret_root, str.c_str(), ret_item);
			
			cJSON_AddStringToObject(ret_item, "ip", ip.c_str());
			cJSON_AddStringToObject(ret_item, "port", std::to_string(port).c_str());
			cJSON_AddStringToObject(ret_item, "user", user.c_str());
			cJSON_AddStringToObject(ret_item, "pwd", pwd.c_str());
		}

		break;
	}

	ret_cjson = cJSON_Print(ret_root);
	str_ret = ret_cjson;

	if(ret_root != NULL)
		cJSON_Delete(ret_root);
	if(ret_cjson != NULL)
		free(ret_cjson);

	return true;
}

bool System::get_variable(cJSON *root, std::string &str_ret)
{
	Scopped_mutex sm(mtx);

	cJSON *ret_root;
	cJSON *item;
	char *ret_cjson;
	std::string variable,ip,result,value;
	int port;

	item = cJSON_GetObjectItem(root, "variable");
	if(item == NULL || item->valuestring == NULL)
	{
		syslog(Logger::ERROR, "get variable error");
		return false;
	}
	variable = item->valuestring;

	item = cJSON_GetObjectItem(root, "ip");
	if(item == NULL || item->valuestring == NULL)
	{
		syslog(Logger::ERROR, "get ip error");
		return false;
	}
	ip = item->valuestring;

	item = cJSON_GetObjectItem(root, "port");
	if(item == NULL || item->valuestring == NULL)
	{
		syslog(Logger::ERROR, "get port error");
		return false;
	}
	port = atoi(item->valuestring);

	for(auto &node: meta_shard.get_nodes())
	{
		if(node->matches_ip_port(ip, port))
		{
			if(node->get_variables(variable, value)==0)
				result = "true";
			else
				result = "false";

			goto end;
		}
	}

	for (auto &cluster:kl_clusters)
	{
		for(auto &shard:cluster->storage_shards)
		{
			for(auto &node:shard->get_nodes())
			{
				if(node->matches_ip_port(ip, port))
				{
					if(node->get_variables(variable, value)==0)
						result = "true";
					else
						result = "false";

					goto end;
				}
			}
		}

		for(auto &comp:cluster->computer_nodes)
		{
			if(comp->matches_ip_port(ip, port))
			{
				if(comp->get_variables(variable, value)==0)
					result = "true";
				else
					result = "false";

				goto end;
			}
		}
	}

	if(result.length()==0)
		result = "false";

end:

	ret_root = cJSON_CreateObject();
	cJSON_AddStringToObject(ret_root, "result", result.c_str());
	cJSON_AddStringToObject(ret_root, "value", value.c_str());

	ret_cjson = cJSON_Print(ret_root);
	str_ret = ret_cjson;

	if(ret_root != NULL)
		cJSON_Delete(ret_root);
	if(ret_cjson != NULL)
		free(ret_cjson);

	return true;
}

bool System::set_variable(cJSON *root, std::string &str_ret)
{
	Scopped_mutex sm(mtx);

	cJSON *ret_root;
	cJSON *item;
	char *ret_cjson;
	std::string variable,ip,result,value_int,value_str;
	int port;

	item = cJSON_GetObjectItem(root, "variable");
	if(item == NULL || item->valuestring == NULL)
	{
		syslog(Logger::ERROR, "get variable error");
		return false;
	}
	variable = item->valuestring;

	item = cJSON_GetObjectItem(root, "value_int");
	if(item == NULL || item->valuestring == NULL)
	{
		item = cJSON_GetObjectItem(root, "value_str");
		if(item == NULL || item->valuestring == NULL)
		{
			syslog(Logger::ERROR, "get value_int & value_str error");
			return false;
		}
		value_str = item->valuestring;
	}
	else
		value_int = item->valuestring;

	item = cJSON_GetObjectItem(root, "ip");
	if(item == NULL || item->valuestring == NULL)
	{
		syslog(Logger::ERROR, "get ip error");
		return false;
	}
	ip = item->valuestring;

	item = cJSON_GetObjectItem(root, "port");
	if(item == NULL || item->valuestring == NULL)
	{
		syslog(Logger::ERROR, "get port error");
		return false;
	}
	port = atoi(item->valuestring);

	for(auto &node: meta_shard.get_nodes())
	{
		if(node->matches_ip_port(ip, port))
		{
			if(node->set_variables(variable, value_int, value_str)==0)
				result = "true";
			else
				result = "false";

			goto end;
		}
	}

	for (auto &cluster:kl_clusters)
	{
		for(auto &shard:cluster->storage_shards)
		{
			for(auto &node:shard->get_nodes())
			{
				if(node->matches_ip_port(ip, port))
				{
					if(node->set_variables(variable, value_int, value_str)==0)
						result = "true";
					else
						result = "false";

					goto end;
				}
			}
		}

		for(auto &comp:cluster->computer_nodes)
		{
			if(comp->matches_ip_port(ip, port))
			{
				if(comp->set_variables(variable, value_int, value_str)==0)
					result = "true";
				else
					result = "false";

				goto end;
			}
		}
	}

	if(result.length()==0)
		result = "false";

end:

	ret_root = cJSON_CreateObject();
	cJSON_AddStringToObject(ret_root, "result", result.c_str());

	ret_cjson = cJSON_Print(ret_root);
	str_ret = ret_cjson;

	if(ret_root != NULL)
		cJSON_Delete(ret_root);
	if(ret_cjson != NULL)
		free(ret_cjson);

	return true;
}

bool System::get_shards_ip_port(std::string &cluster_name, std::vector <std::vector<Tpye_Ip_Port>> &vec_vec_shard)
{
	Scopped_mutex sm(mtx);

	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//get ip and port
		for(auto &shard:cluster->storage_shards)
		{
			std::vector<Tpye_Ip_Port> vec_storage_ip_port;
			for(auto &node:shard->get_nodes())
			{
				std::string ip;
				int port;
				node->get_ip_port(ip, port);
				vec_storage_ip_port.emplace_back(std::make_pair(ip, port));
			}
			vec_vec_shard.emplace_back(vec_storage_ip_port);
		}

		break;
	}

	return true;
}

bool System::get_shards_ip_port(std::string &cluster_name, std::string &shard_name, std::vector<Tpye_Ip_Port> &vec_shard)
{
	Scopped_mutex sm(mtx);

	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//get ip and port
		for(auto &shard:cluster->storage_shards)
		{
			if(shard_name != shard->get_name())
				continue;

			for(auto &node:shard->get_nodes())
			{
				std::string ip;
				int port;
				node->get_ip_port(ip, port);
				vec_shard.emplace_back(std::make_pair(ip, port));
			}

			break;
		}

		break;
	}

	return (vec_shard.size() > 0);
}

bool System::get_comps_ip_port(std::string &cluster_name, std::vector<Tpye_Ip_Port> &vec_comp)
{
	Scopped_mutex sm(mtx);

	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//get ip and port
		for(auto &comp:cluster->computer_nodes)
		{
			std::string ip;
			int port;
			comp->get_ip_port(ip, port);
			vec_comp.emplace_back(std::make_pair(ip, port));
		}
		
		break;
	}

	return true;
}

bool System::get_comps_ip_port(std::string &cluster_name, std::string &comp_name, std::vector<Tpye_Ip_Port> &vec_comp)
{
	Scopped_mutex sm(mtx);

	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//get ip and port
		for(auto &comp:cluster->computer_nodes)
		{
			if(comp_name != comp->get_name())
				continue;

			std::string ip;
			int port;
			comp->get_ip_port(ip, port);
			vec_comp.emplace_back(std::make_pair(ip, port));

			break;
		}
		
		break;
	}

	return true;
}

bool System::update_variables(std::string &cluster_name, std::string &shard_name, Tpye_Ip_Port &ip_port, Tpye_string2 &t_string2)
{
	Scopped_mutex sm(mtx);
	bool ret = false;

	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//get ip and port
		for(auto &shard:cluster->storage_shards)
		{
			if(shard_name != shard->get_name())
				continue;

			for(auto &node:shard->get_nodes())
			{
				if(node->matches_ip_port(ip_port.first, ip_port.second))
				{
					if(node->update_variables(t_string2)==0)
						ret = true;

					break;
				}
			}

			break;
		}

		break;
	}

	return ret;
}

bool System::add_shard_nodes(std::string &cluster_name, std::string &shard_name, std::vector<Tpye_Ip_Port_User_Pwd> vec_ip_port_user_pwd)
{
	Scopped_mutex sm(mtx);

	if(meta_shard.add_shard_nodes(cluster_name, shard_name, vec_ip_port_user_pwd))
	{
		//syslog(Logger::ERROR, "add_shard_nodes error");
		return false;
	}

	return true;
}

bool System::stop_cluster(std::string &cluster_name)
{
	Scopped_mutex sm(mtx);

	for (auto cluster_it=kl_clusters.begin(); cluster_it!=kl_clusters.end(); )
	{
		if(cluster_name != (*cluster_it)->get_name())
		{
			cluster_it++;
			continue;
		}

		//remove storage and shard
		for(auto shard_it=(*cluster_it)->storage_shards.begin(); shard_it!=(*cluster_it)->storage_shards.end(); )
		{
			for(auto node_it=(*shard_it)->get_nodes().begin(); node_it!=(*shard_it)->get_nodes().end(); )
			{
				delete *node_it;
				node_it = (*shard_it)->get_nodes().erase(node_it);
			}
			
			delete *shard_it;
			shard_it = (*cluster_it)->storage_shards.erase(shard_it);
		}

		//then remove computer
		for(auto comp_it=(*cluster_it)->computer_nodes.begin(); comp_it!=(*cluster_it)->computer_nodes.end(); )
		{
			delete *comp_it;
			comp_it = (*cluster_it)->computer_nodes.erase(comp_it);
		}

		//remove cluster
		delete *cluster_it;
		System::get_instance()->kl_clusters.erase(cluster_it);
		
		break;
	}

	if(meta_shard.delete_cluster_from_metadata(cluster_name))
	{
		//syslog(Logger::ERROR, "delete_cluster_from_metadata error");
		return false;
	}

	return true;
}

bool System::stop_cluster_shard(std::string &cluster_name, std::string &shard_name)
{
	Scopped_mutex sm(mtx);

	for (auto cluster_it=kl_clusters.begin(); cluster_it!=kl_clusters.end(); )
	{
		if(cluster_name != (*cluster_it)->get_name())
		{
			cluster_it++;
			continue;
		}

		//remove storage and shard by shard_name
		for(auto shard_it=(*cluster_it)->storage_shards.begin(); shard_it!=(*cluster_it)->storage_shards.end(); )
		{
			if(shard_name != (*shard_it)->get_name())
			{
				shard_it++;
				continue;
			}

			for(auto node_it=(*shard_it)->get_nodes().begin(); node_it!=(*shard_it)->get_nodes().end(); )
			{
				delete *node_it;
				node_it = (*shard_it)->get_nodes().erase(node_it);
			}
			
			delete *shard_it;
			shard_it = (*cluster_it)->storage_shards.erase(shard_it);

			break;
		}
		
		break;
	}

	if(meta_shard.delete_cluster_shard_from_metadata(cluster_name, shard_name))
	{
		//syslog(Logger::ERROR, "delete_cluster_shard_from_metadata error");
		return false;
	}

	return true;
}

bool System::stop_cluster_shard_node(std::string &cluster_name, std::string &shard_name, Tpye_Ip_Port &ip_port)
{
	Scopped_mutex sm(mtx);

	std::string ip;
	int port;

	for (auto cluster_it=kl_clusters.begin(); cluster_it!=kl_clusters.end(); )
	{
		if(cluster_name != (*cluster_it)->get_name())
		{
			cluster_it++;
			continue;
		}

		//remove shard node
		for(auto shard_it=(*cluster_it)->storage_shards.begin(); shard_it!=(*cluster_it)->storage_shards.end(); )
		{
			if(shard_name != (*shard_it)->get_name())
			{
				shard_it++;
				continue;
			}

			for(auto node_it=(*shard_it)->get_nodes().begin(); node_it!=(*shard_it)->get_nodes().end(); node_it++)
			{
				if((*node_it)->matches_ip_port(ip_port.first, ip_port.second))
				{
					(*shard_it)->remove_node((*node_it)->get_id());
					break;
				}
			}

			break;
		}
		
		break;
	}

	if(meta_shard.delete_cluster_shard_node_from_metadata(cluster_name, shard_name, ip_port))
	{
		//syslog(Logger::ERROR, "delete_cluster_shard_node_from_metadata error");
		return false;
	}

	return true;
}

bool System::stop_cluster_comp(std::string &cluster_name, std::string &comp_name)
{
	Scopped_mutex sm(mtx);

	for (auto cluster_it=kl_clusters.begin(); cluster_it!=kl_clusters.end(); )
	{
		if(cluster_name != (*cluster_it)->get_name())
		{
			cluster_it++;
			continue;
		}

		//remove comp and shard by comp_name
		for(auto comp_it=(*cluster_it)->computer_nodes.begin(); comp_it!=(*cluster_it)->computer_nodes.end(); )
		{
			if(comp_name != (*comp_it)->get_name())
			{
				comp_it++;
				continue;
			}
			
			delete *comp_it;
			comp_it = (*cluster_it)->computer_nodes.erase(comp_it);

			break;
		}
		
		break;
	}

	if(meta_shard.delete_cluster_comp_from_metadata(cluster_name, comp_name))
	{
		//syslog(Logger::ERROR, "delete_cluster_comp_from_metadata error");
		return false;
	}

	return true;
}

bool System::get_shard_ip_port_backup(std::string &cluster_name, std::vector<Tpye_Ip_Port> &vec_ip_port)
{
	Scopped_mutex sm(mtx);

	std::string ip;
	int port;

	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//sort shard by id
		sort(cluster->storage_shards.begin(), cluster->storage_shards.end(), 
				[](Shard * a, Shard * b){return a->get_id() < a->get_id();});

		//get a no master node for backup in every shard
		for (auto &shard:cluster->storage_shards)
		{
			Shard_node *cur_master = shard->get_master();
			Shard_node *backup_node = NULL;
			auto vec_node = shard->get_nodes();
			if(vec_node.size() == 0)
			{
				return false;
			}
			else if(vec_node.size() == 1)
			{
				backup_node = vec_node[0];
			}
			else
			{
				for(auto node:vec_node)
					if(cur_master!=node)
					{
						backup_node = node;
						break;
					}
			}

			if(backup_node == NULL)
				return false;

			backup_node->get_ip_port(ip, port);
			vec_ip_port.emplace_back(std::make_pair(ip, port));
		}

		break;
	}

	return (vec_ip_port.size() > 0);
}

bool System::get_shard_ip_port_restore(std::string &cluster_name, std::vector<std::vector<Tpye_Ip_Port>> &vec_vec_ip_port)
{
	Scopped_mutex sm(mtx);

	std::string ip;
	int port;

	//storage node
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//sort shard by id
		sort(cluster->storage_shards.begin(), cluster->storage_shards.end(), 
				[](Shard * a, Shard * b){return a->get_id() < a->get_id();});

		//get every node for restore in every shard
		for (auto &shard:cluster->storage_shards)
		{
			std::vector<Tpye_Ip_Port> vec_ip_port;
			for (auto &node:shard->get_nodes())
			{
				node->get_ip_port(ip, port);
				vec_ip_port.emplace_back(std::make_pair(ip, port));
			}
			vec_vec_ip_port.emplace_back(vec_ip_port);
		}

		break;
	}

	return (vec_vec_ip_port.size() > 0);
}

bool System::get_comps_ip_port_restore(std::string &cluster_name, std::vector<Tpye_Ip_Port> &vec_ip_port)
{
	Scopped_mutex sm(mtx);

	std::string ip;
	int port;

	//computer node
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//get every node for restore
		for (auto &node:cluster->computer_nodes)
		{
			node->get_ip_port(ip, port);
			vec_ip_port.emplace_back(std::make_pair(ip, port));
		}

		break;
	}

	return (vec_ip_port.size() > 0);
}

bool System::get_cluster_shards_nodes_comps(std::string &cluster_name, int &shards, int &nodes, int &comps)
{
	Scopped_mutex sm(mtx);
	bool ret = false;

	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		shards = cluster->storage_shards.size();
		if(shards == 0)
			return false;

		nodes = cluster->storage_shards[0]->get_nodes().size();
		comps = cluster->computer_nodes.size();
		ret = true;

		break;
	}

	return ret;
}

bool System::get_cluster_mgr_mode(std::string &cluster_name)
{
	Scopped_mutex sm(mtx);
	bool ret = false;

	//storage node
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//get mgr mode from master of shard
		for (auto &shard:cluster->storage_shards)
		{
			if(shard->get_mode() == Shard::HAVL_mode::HA_mgr)
				ret = true;
			break;
		}

		break;
	}

	return ret;
}

bool System::clear_cluster_shard_master(std::string &cluster_name)
{
	Scopped_mutex sm(mtx);

	//storage node
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//set every shard master to NULL
		for (auto &shard:cluster->storage_shards)
		{
			shard->set_master(NULL);
		}

		break;
	}

	return true;
}

bool System::update_instance_cluster_info(std::string &cluster_name)
{
	Scopped_mutex sm(mtx);
	bool ret = true;

	//storage node
	for (auto &cluster:kl_clusters)
	{
		if(cluster_name != cluster->get_name())
			continue;

		//check every shard master
		for (auto &shard:cluster->storage_shards)
		{
			if(shard->get_master() == NULL)
			{
				ret = false;
				break;
			}
		}

		//update cluster_info by master of shard
		if(ret)
		{
			syslog(Logger::INFO, "every shard have get master");
			for (auto &shard:cluster->storage_shards)
			{
				if(shard->get_master()->update_instance_cluster_info())
				{
					syslog(Logger::ERROR, "update_instance_cluster_info error");
					ret = false;
					break;
				}
			}
		}

		break;
	}

	return ret;
}