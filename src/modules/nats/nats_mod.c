/*
 * NATS module interface
 *
 * Copyright (C) 2021 Voxcom Inc
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 */

#include "nats_mod.h"

MODULE_VERSION

init_nats_sub_ptr _init_nats_sc = NULL;
init_nats_server_ptr _init_nats_srv = NULL;
nats_consumer_worker_t *nats_workers = NULL;
int _nats_proc_count;
char *eventData = NULL;

static pv_export_t nats_mod_pvs[] = {
		{{"natsData", (sizeof("natsData") - 1)}, PVT_OTHER,
				nats_pv_get_event_payload, 0, 0, 0, 0, 0},
		{{0, 0}, 0, 0, 0, 0, 0, 0, 0}};

static param_export_t params[] = {{"nats_url", PARAM_STRING | USE_FUNC_PARAM,
										  (void *)_init_nats_server_url_add},
		{"subject_queue_group", PARAM_STRING | USE_FUNC_PARAM,
				(void *)_init_nats_sub_add}};

struct module_exports exports = {
		"nats", DEFAULT_DLFLAGS, /* dlopen flags */
		0,						 /* Exported functions */
		params,					 /* Exported parameters */
		0,						 /* exported MI functions */
		nats_mod_pvs,			 /* exported pseudo-variables */
		0,						 /* response function*/
		mod_init,				 /* module initialization function */
		mod_child_init,			 /* per-child init function */
		mod_destroy				 /* destroy function */
};

static void onMsg(
		natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure)
{
	nats_on_message_ptr on_message = (nats_on_message_ptr)closure;
	char *s = (char *)natsMsg_GetSubject(msg);
	char *data = (char *)natsMsg_GetData(msg);
	if(on_message->rt < 0 || event_rt.rlist[on_message->rt] == NULL) {
		LM_INFO("event-route [nats:%s] does not exist\n", s);
		goto end;
	}
	eventData = data;
	nats_run_cfg_route(on_message->rt);

end:
	eventData = NULL;
	natsMsg_Destroy(msg);
}

static void connectedCB(natsConnection *nc, void *closure)
{
	char url[NATS_URL_MAX_SIZE];
	natsConnection_GetConnectedUrl(nc, url, sizeof(url));
	nats_run_cfg_route(_nats_rts.connected);
}

static void disconnectedCb(natsConnection *nc, void *closure)
{
	char url[NATS_URL_MAX_SIZE];
	natsConnection_GetConnectedUrl(nc, url, sizeof(url));
	nats_run_cfg_route(_nats_rts.disconnected);
}

static void reconnectedCb(natsConnection *nc, void *closure)
{
	char url[NATS_URL_MAX_SIZE];
	natsConnection_GetConnectedUrl(nc, url, sizeof(url));
	nats_run_cfg_route(_nats_rts.connected);
}

static void closedCB(natsConnection *nc, void *closure)
{
	bool *closed = (bool *)closure;
	const char *err = NULL;
	natsConnection_GetLastError(nc, &err);
	LM_INFO("connect failed: %s\n", err);
	*closed = true;
}

void nats_consumer_worker_proc(
		nats_consumer_worker_t *worker, const char *servers[])
{
	natsStatus s;
	bool closed = false;
	int i;

	LM_INFO("nats worker connecting to subject [%s] queue group [%s]\n",
			worker->subject, worker->queue_group);

	s = natsOptions_Create(&worker->opts);
	if(s != NATS_OK) {
		LM_ERR("could not create nats options [%s]\n", natsStatus_GetText(s));
		return;
	}
	// use these defaults
	natsOptions_SetAllowReconnect(worker->opts, true);
	natsOptions_SetSecure(worker->opts, false);
	natsOptions_SetMaxReconnect(worker->opts, 10000);
	natsOptions_SetReconnectWait(worker->opts, 2 * 1000);	  // 2s
	natsOptions_SetPingInterval(worker->opts, 2 * 60 * 1000); // 2m
	natsOptions_SetMaxPingsOut(worker->opts, 2);
	natsOptions_SetIOBufSize(worker->opts, 32 * 1024); // 32 KB
	natsOptions_SetMaxPendingMsgs(worker->opts, 65536);
	natsOptions_SetTimeout(worker->opts, 2 * 1000);					// 2s
	natsOptions_SetReconnectBufSize(worker->opts, 8 * 1024 * 1024); // 8 MB;
	natsOptions_SetReconnectJitter(worker->opts, 100, 1000); // 100ms, 1s;
	s = natsOptions_SetDisconnectedCB(worker->opts, disconnectedCb, NULL);
	if(s != NATS_OK) {
		LM_ERR("could not set disconnect callback [%s]\n",
				natsStatus_GetText(s));
	}
	s = natsOptions_SetReconnectedCB(worker->opts, reconnectedCb, NULL);
	if(s != NATS_OK) {
		LM_ERR("could not set reconnect callback [%s]\n",
				natsStatus_GetText(s));
	}
	s = natsOptions_SetRetryOnFailedConnect(
			worker->opts, true, connectedCB, NULL);
	if(s != NATS_OK) {
		LM_ERR("could not set retry on failed callback [%s]\n",
				natsStatus_GetText(s));
	}
	s = natsOptions_SetClosedCB(worker->opts, closedCB, (void *)&closed);
	if(s != NATS_OK) {
		LM_ERR("could not set closed callback [%s]\n", natsStatus_GetText(s));
	}

	i = 0;
	while (servers[i] != NULL) {
		s = natsOptions_SetServers(worker->opts, &servers[i], 1);
		if(s != NATS_OK) {
			LM_ERR("could not set nats server %s [%s]\n", servers[i], natsStatus_GetText(s));
		}
		s = natsConnection_Connect(&worker->conn[i], worker->opts);
		if(s != NATS_OK) {
			LM_ERR("could not connect %s [%s]\n", servers[i], natsStatus_GetText(s));
		}
		i++;
	}
	// create a loop
	natsLibuv_Init();
	worker->uvLoop = uv_default_loop();
	if(worker->uvLoop != NULL) {
		natsLibuv_SetThreadLocalLoop(worker->uvLoop);
	} else {
		s = NATS_ERR;
	}

	s = natsOptions_SetEventLoop(worker->opts, (void *)worker->uvLoop,
			natsLibuv_Attach, natsLibuv_Read, natsLibuv_Write,
			natsLibuv_Detach);
	if(s != NATS_OK) {
		LM_ERR("could not set event loop [%s]\n", natsStatus_GetText(s));
	}

	if(s) {
		LM_ERR("error setting options [%s]\n", natsStatus_GetText(s));
	}

	i = 0;
	while (servers[i] != NULL) {
		s = natsConnection_QueueSubscribe(&worker->subscription, worker->conn[i],
				worker->subject, worker->queue_group, onMsg, worker->on_message);
		if(s != NATS_OK) {
			LM_ERR("could not subscribe %s [%s]\n", servers[i], natsStatus_GetText(s));
		}
		i++;
	}

	s = natsSubscription_SetPendingLimits(worker->subscription, -1, -1);
	if(s != NATS_OK) {
		LM_ERR("could not set pending limits [%s]\n", natsStatus_GetText(s));
	}

	// Run the event loop.
	// This call will return when the connection is closed (either after
	// receiving all messages, or disconnected and unable to reconnect).
	if(s == NATS_OK) {
		uv_run(worker->uvLoop, UV_RUN_DEFAULT);
	}
	if(s != NATS_OK) {
		LM_ERR("nats error [%s]\n", natsStatus_GetText(s));
	}
}

static int mod_init(void)
{
	if(faked_msg_init() < 0) {
		LM_ERR("failed to init faked sip message\n");
		return -1;
	}
	nats_init_environment();
	register_procs(_nats_proc_count);
	nats_workers =
			shm_malloc(_nats_proc_count * sizeof(nats_consumer_worker_t));
	if(nats_workers == NULL) {
		LM_ERR("error in shm_malloc\n");
		return -1;
	}
	memset(nats_workers, 0, _nats_proc_count * sizeof(nats_consumer_worker_t));
	return 0;
}

int init_worker(
		nats_consumer_worker_t *worker, char *subject, char *queue_group)
{
	int buffsize = strlen(subject) + 6;
	char routename[buffsize];
	int rt;
	int len;
	char *sc;
	int num_servers = 0;
	init_nats_server_ptr s0;

	memset(worker, 0, sizeof(*worker));
	worker->subject = shm_malloc(strlen(subject) + 1);
	strcpy(worker->subject, subject);
	worker->subject[strlen(subject)] = '\0';
	worker->queue_group = shm_malloc(strlen(queue_group) + 1);
	strcpy(worker->queue_group, queue_group);
	worker->queue_group[strlen(queue_group)] = '\0';
	memset(worker->init_nats_servers, 0, sizeof(worker->init_nats_servers));
	worker->on_message =
			(nats_on_message_ptr)shm_malloc(sizeof(nats_on_message));
	memset(worker->on_message, 0, sizeof(nats_on_message));

	s0 = _init_nats_srv;
	while(s0) {
		if(s0->url != NULL && num_servers < NATS_MAX_SERVERS) {
			len = strlen(s0->url);
			sc = shm_malloc(len + 1);
			if(!sc) {
				LM_ERR("no shm memory left\n");
				return -1;
			}
			strcpy(sc, s0->url);
			sc[len] = '\0';
			worker->init_nats_servers[num_servers++] = sc;
		}
		s0 = s0->next;
	}
	if(num_servers == 0) {
		worker->init_nats_servers[0] = NATS_DEFAULT_URL;
		LM_INFO("using default server [%s]\n", NATS_DEFAULT_URL);
	}

	snprintf(routename, buffsize, "nats:%s", subject);
	routename[buffsize] = '\0';

	rt = route_get(&event_rt, routename);
	if(rt < 0 || event_rt.rlist[rt] == NULL) {
		LM_INFO("route [%s] does not exist\n", routename);
		worker->on_message->rt = -1;
		return 0;
	}
	worker->on_message->rt = rt;
	return 0;
}

void worker_loop(int id)
{
	nats_consumer_worker_t *worker = &nats_workers[id];
	nats_consumer_worker_proc(worker, (const char **)worker->init_nats_servers);
	for(;;) {
		sleep(1000);
	}
}

/**
 * @brief Initialize async module children
 */
static int mod_child_init(int rank)
{
	init_nats_sub_ptr n;
	int i = 0;
	int newpid;

	if(rank == PROC_INIT) {
		n = _init_nats_sc;
		while(n) {
			if(init_worker(&nats_workers[i], n->sub, n->queue_group) < 0) {
				LM_ERR("failed to init struct for worker[%d]\n", i);
				return -1;
			}
			n = n->next;
			i++;
		}
		if(nats_cleanup_init_sub() < 0) {
			LM_INFO("could not cleanup init data\n");
		}
		if(nats_cleanup_init_servers() < 0) {
			LM_INFO("could not cleanup init server data\n");
		}
		return 0;
	}

	if(rank == PROC_MAIN) {
		for(i = 0; i < _nats_proc_count; i++) {
			newpid = fork_process(PROC_RPC, "NATS WORKER", 1);
			if(newpid < 0) {
				LM_ERR("failed to fork worker process %d\n", i);
				return -1;
			} else if(newpid == 0) {
				worker_loop(i);
			} else {
				nats_workers[i].pid = newpid;
			}
		}
		return 0;
	}

	return 0;
}

int nats_cleanup_init_sub()
{
	init_nats_sub_ptr n0;
	init_nats_sub_ptr n1;
	n0 = _init_nats_sc;
	while(n0) {
		n1 = n0->next;
		if(n0->sub != NULL) {
			shm_free(n0->sub);
		}
		if(n0->queue_group != NULL) {
			shm_free(n0->queue_group);
		}
		shm_free(n0);
		n0 = n1;
	}
	_init_nats_sc = NULL;
	return 0;
}

int nats_cleanup_init_servers()
{
	init_nats_server_ptr s0;
	init_nats_server_ptr s1;
	s0 = _init_nats_srv;
	while(s0) {
		s1 = s0->next;
		if(s0->url != NULL) {
			shm_free(s0->url);
		}
		shm_free(s0);
		s0 = s1;
	}
	_init_nats_srv = NULL;
	return 0;
}

int nats_destroy_workers()
{
	int i, j;
	int s;
	nats_consumer_worker_t *worker;
	for(i = 0; i < _nats_proc_count; i++) {
		worker = &nats_workers[i];
		if(worker != NULL) {
			if(worker->subscription != NULL) {
				natsSubscription_Unsubscribe(worker->subscription);
				natsSubscription_Destroy(worker->subscription);
			}
			j = 0;
			while (worker->conn[j] != NULL) {
				natsConnection_Close(worker->conn[j]);
				natsConnection_Destroy(worker->conn[j]);
				j++;
			}
			if(worker->opts != NULL) {
				natsOptions_Destroy(worker->opts);
			}
			if(worker->uvLoop != NULL) {
				uv_loop_close(worker->uvLoop);
			}
			nats_Close();
			if(worker->subject != NULL) {
				shm_free(worker->subject);
			}
			if(worker->queue_group != NULL) {
				shm_free(worker->queue_group);
			}
			if(worker->on_message != NULL) {
				shm_free(worker->on_message);
			}
			for(s = 0; s < NATS_MAX_SERVERS; s++) {
				if(worker->init_nats_servers[s]) {
					shm_free(worker->init_nats_servers[s]);
				}
			}
			shm_free(worker);
		}
	}
	return 0;
}

/**
 * destroy module function
 */
static void mod_destroy(void)
{
	if(nats_destroy_workers() < 0) {
		LM_ERR("could not cleanup workers\n");
	}
}

int _init_nats_server_url_add(modparam_t type, void *val)
{
	char *url = (char *)val;
	int len = strlen(url);
	char *value;
	if(len > NATS_URL_MAX_SIZE) {
		LM_ERR("connection url exceeds max size %d\n", NATS_URL_MAX_SIZE);
		return -1;
	}
	if(strncmp(url, "nats://", 7)) {
		LM_ERR("invalid nats url [%s]\n", url);
		return -1;
	}
	value = pkg_malloc(len + 1);
	if(!value) {
		LM_ERR("no pkg memory left\n");
		return -1;
	}
	strcpy(value, url);
	value[len] = '\0';
	if(init_nats_server_url_add(url) < 0) {
		LM_ERR("could not add server\n");
	}
	pkg_free(value);
	return 0;
}

int _init_nats_sub_add(modparam_t type, void *val)
{
	char *sub = (char *)val;
	int len = strlen(sub);
	char *s = pkg_malloc(len + 1);
	if(!s) {
		LM_ERR("no pkg memory left\n");
		return -1;
	}

	strcpy(s, sub);
	s[len] = '\0';
	if(init_nats_sub_add(s) < 0) {
		LM_ERR("could not add init data\n");
	}
	pkg_free(s);
	return 0;
}

/**
 * Invoke a event route block
 */
int nats_run_cfg_route(int rt)
{
	struct run_act_ctx ctx;
	sip_msg_t *fmsg;
	sip_msg_t tmsg;

	// check for valid route pointer
	if(rt < 0) {
		return 0;
	}

	fmsg = faked_msg_next();
	memcpy(&tmsg, fmsg, sizeof(sip_msg_t));
	fmsg = &tmsg;
	set_route_type(EVENT_ROUTE);
	init_run_actions_ctx(&ctx);
	run_top_route(event_rt.rlist[rt], fmsg, 0);
	return 0;
}

void nats_init_environment()
{
	memset(&_nats_rts, 0, sizeof(nats_evroutes_t));
	_nats_rts.connected = route_lookup(&event_rt, "nats:connected");
	if(_nats_rts.connected < 0 || event_rt.rlist[_nats_rts.connected] == NULL)
		_nats_rts.connected = -1;

	_nats_rts.disconnected = route_lookup(&event_rt, "nats:disconnected");
	if(_nats_rts.disconnected < 0
			|| event_rt.rlist[_nats_rts.disconnected] == NULL)
		_nats_rts.disconnected = -1;
}

init_nats_server_ptr _init_nats_server_list_new(char *url)
{
	init_nats_server_ptr p =
			(init_nats_server_ptr)shm_malloc(sizeof(init_nats_server));
	memset(p, 0, sizeof(init_nats_server));
	p->url = shm_malloc(strlen(url) + 1);
	strcpy(p->url, url);
	p->url[strlen(url)] = '\0';
	return p;
}

int init_nats_server_url_add(char *url)
{
	init_nats_server_ptr n;
	n = _init_nats_srv;
	while(n != NULL) {
		n = n->next;
	}
	n = _init_nats_server_list_new(url);
	n->next = _init_nats_srv;
	_init_nats_srv = n;
	return 0;
}

init_nats_sub_ptr _init_nats_sub_new(char *sub, char *queue_group)
{
	init_nats_sub_ptr p = (init_nats_sub_ptr)shm_malloc(sizeof(init_nats_sub));
	memset(p, 0, sizeof(init_nats_sub));
	p->sub = shm_malloc(strlen(sub) + 1);
	strcpy(p->sub, sub);
	p->sub[strlen(sub)] = '\0';
	p->queue_group = shm_malloc(strlen(queue_group) + 1);
	strcpy(p->queue_group, queue_group);
	p->queue_group[strlen(queue_group)] = '\0';
	return p;
}

int init_nats_sub_add(char *sc)
{
	int len;
	char *s;
	char *c;
	init_nats_sub_ptr n;

	if(sc == NULL) {
		return -1;
	}

	len = strlen(sc);
	s = pkg_malloc(len + 1);
	if(!s) {
		LM_ERR("no pkg memory left\n");
		return -1;
	}
	strcpy(s, sc);
	s[len] = '\0';

	if((c = strchr(s, ':')) != 0) {
		*c = 0;
		for(c = c + 1; !*c; c++)
			;
	}
	if(s == NULL) {
		goto error;
		return -1;
	}
	if(c == NULL) {
		goto error;
		return -1;
	}

	n = _init_nats_sc;
	while(n != NULL) {
		n = n->next;
	}
	n = _init_nats_sub_new(s, c);
	n->next = _init_nats_sc;
	_init_nats_sc = n;
	_nats_proc_count++;


error:
	pkg_free(s);
	return 0;
}

int nats_pv_get_event_payload(
		struct sip_msg *msg, pv_param_t *param, pv_value_t *res)
{
	return eventData == NULL ? pv_get_null(msg, param, res)
							 : pv_get_strzval(msg, param, res, eventData);
}
