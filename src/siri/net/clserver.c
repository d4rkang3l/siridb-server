/*
 * clserver.c - TCP server for serving client requests.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 09-03-2016
 *
 */
#define _GNU_SOURCE
#include <siri/net/clserver.h>
#include <siri/net/protocol.h>
#include <logger/logger.h>
#include <stdlib.h>
#include <string.h>
#include <msgpack.h>
#include <stdbool.h>
#include <stdio.h>
#include <siri/siri.h>
#include <siri/db/auth.h>
#include <siri/db/query.h>
#include <qpack/qpack.h>
#include <siri/db/insert.h>
#include <siri/net/socket.h>
#include <assert.h>
#include <siri/version.h>
#include <lock/lock.h>
#include <siri/db/servers.h>
#include <siri/db/users.h>
#include <siri/net/promises.h>
#include <siri/err.h>
#include <siri/db/replicate.h>

#define DEFAULT_BACKLOG 128
#define CHECK_SIRIDB(ssocket)                                               \
sirinet_socket_t * ssocket = client->data;                                  \
if (ssocket->siridb == NULL)                                                \
{                                                                           \
    sirinet_pkg_t * package;                                                \
    package = sirinet_pkg_new(pkg->pid, 0, CPROTO_ERR_NOT_AUTHENTICATED, NULL); \
    if (package != NULL)                                                    \
    {                                                                       \
        sirinet_pkg_send((uv_stream_t *) client, package);                  \
        free(package);                                                      \
    }                                                                       \
    return;                                                                 \
}

static const int SERVER_RUNNING_REINDEXING =
        SERVER_FLAG_RUNNING + SERVER_FLAG_REINDEXING;

static uv_loop_t * loop = NULL;
static struct sockaddr_in client_addr;
static uv_tcp_t client_server;

static void on_data(uv_stream_t * client, sirinet_pkg_t * pkg);
static void on_new_connection(uv_stream_t * server, int status);
static void on_auth_request(uv_stream_t * client, sirinet_pkg_t * pkg);
static void on_query(uv_stream_t * client, sirinet_pkg_t * pkg);
static void on_insert(uv_stream_t * client, sirinet_pkg_t * pkg);
static void on_ping(uv_stream_t * client, sirinet_pkg_t * pkg);
static void on_info(uv_stream_t * client, sirinet_pkg_t * pkg);
static void on_loaddb(uv_stream_t * client, sirinet_pkg_t * pkg);
static void on_reqfile(
        uv_stream_t * client,
        sirinet_pkg_t * pkg,
        sirinet_clserver_getfile getfile);
static void on_register_server(uv_stream_t * client, sirinet_pkg_t * pkg);
static void CLSERVER_send_server_error(
        siridb_t * siridb,
        uv_stream_t * stream,
        sirinet_pkg_t * pkg);
static void CLSERVER_send_pool_error(
        uv_stream_t * stream,
        sirinet_pkg_t * pkg);
static int CLSERVER_on_info_cb(siridb_t * siridb, qp_packer_t * packer);
static void CLSERVER_on_register_server_response(
        slist_t * promises,
        siridb_server_reg_t * server_reg);

#define POOL_ERR_MSG \
    "At least one pool has no server available to process the request"

#define POOL_ERR_LEN 64  // exact length of POOL_ERR_MSG


int sirinet_clserver_init(siri_t * siri)
{
#ifdef DEBUG
    assert (loop == NULL && siri->loop != NULL);
#endif

    int rc;

    /* bind loop to the given loop */
    loop = siri->loop;

    uv_tcp_init(loop, &client_server);

    uv_ip4_addr(
            siri->cfg->listen_client_address,
            siri->cfg->listen_client_port,
            &client_addr);

    /* make sure data is set to NULL so we later on can check this value. */
    client_server.data = NULL;

    uv_tcp_bind(&client_server, (const struct sockaddr *) &client_addr, 0);

    rc = uv_listen(
            (uv_stream_t*) &client_server,
            DEFAULT_BACKLOG,
            on_new_connection);

    if (rc)
    {
        log_error("Error listening client server: %s", uv_strerror(rc));
        return 1;
    }

    log_info("Start listening for client connections on '%s:%d'",
            siri->cfg->listen_client_address,
            siri->cfg->listen_client_port);

    return 0;
}

static void on_new_connection(uv_stream_t * server, int status)
{
    log_debug("Received a client connection request.");

    if (status < 0)
    {
        log_error("Client connection error: %s", uv_strerror(status));
        return;
    }
    uv_tcp_t * client =
            sirinet_socket_new(SOCKET_CLIENT, (on_data_cb_t) &on_data);

    uv_tcp_init(loop, client);

    if (uv_accept(server, (uv_stream_t*) client) == 0)
    {
        uv_read_start(
                (uv_stream_t *) client,
                sirinet_socket_alloc_buffer,
                sirinet_socket_on_data);
    }
    else
    {
        uv_close((uv_handle_t *) client, (uv_close_cb) sirinet_socket_free);
    }
}

static void on_data(uv_stream_t * client, sirinet_pkg_t * pkg)
{
#ifdef DEBUG
    log_debug("[Client server] Got data (pid: %d, len: %d, tp: %d)",
            pkg->pid, pkg->len, pkg->tp);
#endif

    /* in case the online flag is not set, we cannot perform any request */
    if (siri.status == SIRI_STATUS_RUNNING)
    {
        switch ((cproto_client_t) pkg->tp)
        {
        case CPROTO_REQ_QUERY:
            on_query(client, pkg);
            break;
        case CPROTO_REQ_INSERT:
            on_insert(client, pkg);
            break;
        case CPROTO_REQ_AUTH:
            on_auth_request(client, pkg);
            break;
        case CPROTO_REQ_PING:
            on_ping(client, pkg);
            break;
        case CPROTO_REQ_INFO:
            on_info(client, pkg);
            break;
        case CPROTO_REQ_LOADDB:
            on_loaddb(client, pkg);
            break;
        case CPROTO_REQ_REGISTER_SERVER:
            on_register_server(client, pkg);
            break;
        case CPROTO_REQ_FILE_SERVERS:
            on_reqfile(client, pkg, siridb_servers_get_file);
            break;
        case CPROTO_REQ_FILE_USERS:
            on_reqfile(client, pkg, siridb_users_get_file);
            break;
        }
    }
    else
    {
        /* data->siridb can be NULL here, make sure we can handle this state */
        CLSERVER_send_server_error(
                ((sirinet_socket_t *) client->data)->siridb,
                client,
                pkg);
    }
}

static void on_auth_request(uv_stream_t * client, sirinet_pkg_t * pkg)
{
    cproto_server_t rc;
    sirinet_pkg_t * package;
    qp_unpacker_t * unpacker = qp_unpacker_new(pkg->data, pkg->len);
    qp_obj_t * qp_username = qp_object_new();
    qp_obj_t * qp_password = qp_object_new();
    qp_obj_t * qp_dbname = qp_object_new();


    if (    qp_is_array(qp_next(unpacker, NULL)) &&
            qp_next(unpacker, qp_username) == QP_RAW &&
            qp_next(unpacker, qp_password) == QP_RAW &&
            qp_next(unpacker, qp_dbname) == QP_RAW)
    {
        rc = siridb_auth_user_request(
                client,
                qp_username,
                qp_password,
                qp_dbname);
        package = sirinet_pkg_new(pkg->pid, 0, rc, NULL);

        /* ignore result code, signal can be raised */
        sirinet_pkg_send(client, package);
        free(package);
    }
    else
    {
        log_error("Invalid 'on_auth_request' received.");
    }
    qp_object_free(qp_username);
    qp_object_free(qp_password);
    qp_object_free(qp_dbname);
    qp_unpacker_free(unpacker);
}

/*
 * A signal is raised in case an allocation error occurred.
 */
static void CLSERVER_send_server_error(
        siridb_t * siridb,
        uv_stream_t * stream,
        sirinet_pkg_t * pkg)
{
    /* WARNING: siridb can be NULL here */

    char * err_msg;
    int len = (siridb == NULL) ?
            asprintf(
            &err_msg,
            "Not accepting the request because the sever is not running")
            :
            asprintf(
            &err_msg,
            "Server '%s' is not accepting then request because of having "
            "status: %u",
            siridb->server->name,
            siridb->server->flags);

    if (len < 0)
    {
        ERR_ALLOC
    }
    else
    {
        log_debug(err_msg);

        sirinet_pkg_t * package = sirinet_pkg_err(
                pkg->pid,
                len,
                CPROTO_ERR_SERVER,
                err_msg);
        if (package != NULL)
        {
            /* ignore result code, signal can be raised */
            sirinet_pkg_send(stream, package);

            /* free package and err_msg*/
            free(package);
        }
        free(err_msg);
    }
}

/*
 * A signal is raised in case an allocation error occurred.
 */
static void CLSERVER_send_pool_error(
        uv_stream_t * stream,
        sirinet_pkg_t * pkg)
{
    log_debug(POOL_ERR_MSG);

    sirinet_pkg_t * package = sirinet_pkg_err(
            pkg->pid,
            POOL_ERR_LEN,
            CPROTO_ERR_POOL,
            POOL_ERR_MSG);

    if (package != NULL)
    {
        /* ignore result code, signal can be raised */
        sirinet_pkg_send(stream, package);

        /* free package and err_msg*/
        free(package);
    }
}

static void on_query(uv_stream_t * client, sirinet_pkg_t * pkg)
{
    CHECK_SIRIDB(ssocket)

    qp_unpacker_t * unpacker = qp_unpacker_new(pkg->data, pkg->len);
    qp_obj_t * qp_query = qp_object_new();
    qp_obj_t * qp_time_precision = qp_object_new();
    siridb_timep_t tp = SIRIDB_TIME_DEFAULT;

    if (    qp_is_array(qp_next(unpacker, NULL)) &&
            qp_next(unpacker, qp_query) == QP_RAW &&
            qp_next(unpacker, qp_time_precision))
    {
        if (qp_time_precision->tp == QP_INT64 &&
                (tp = (siridb_timep_t) qp_time_precision->via->int64) !=
                ssocket->siridb->time->precision)
        {
            tp %= SIRIDB_TIME_END;
        }

        siridb_query_run(
                pkg->pid,
                client,
                qp_query->via->raw,
                qp_query->len,
                tp,
                SIRIDB_QUERY_FLAG_MASTER);
    }
    qp_object_free(qp_query);
    qp_object_free(qp_time_precision);
    qp_unpacker_free(unpacker);
}

static void on_insert(uv_stream_t * client, sirinet_pkg_t * pkg)
{
    CHECK_SIRIDB(ssocket)

    siridb_t * siridb = ssocket->siridb;

    /* only when when the flag is EXACTLY running or
     * running + re-indexing we can continue */
    if (    siridb->server->flags != SERVER_FLAG_RUNNING &&
            siridb->server->flags != SERVER_RUNNING_REINDEXING)
    {
        CLSERVER_send_server_error(siridb, (uv_stream_t *) client, pkg);
        return;
    }

    if (!siridb_pools_reindexing(siridb))
    {
        CLSERVER_send_pool_error(client, pkg);
        return;
    }

    qp_unpacker_t * unpacker = qp_unpacker_new(pkg->data, pkg->len);
    if (unpacker == NULL)
    {
        return;  /* signal is raised */
    }
    qp_obj_t * qp_obj = qp_object_new();
    if (qp_obj == NULL)
    {
        free(unpacker);
        return;
    }
    qp_packer_t * packer[siridb->pools->len];
    siridb_insert_err_t rc;

    const char * err_msg;

    rc = siridb_insert_assign_pools(siridb, unpacker, qp_obj, packer);

    switch (rc)
    {
    case ERR_EXPECTING_ARRAY:
    case ERR_EXPECTING_SERIES_NAME:
    case ERR_EXPECTING_MAP_OR_ARRAY:
    case ERR_EXPECTING_INTEGER_TS:
    case ERR_TIMESTAMP_OUT_OF_RANGE:
    case ERR_UNSUPPORTED_VALUE:
    case ERR_EXPECTING_AT_LEAST_ONE_POINT:
    case ERR_EXPECTING_NAME_AND_POINTS:
        /* something went wrong, get correct err message */
        err_msg = siridb_insert_err_msg(rc);

        /* create and send package */
        sirinet_pkg_t * package = sirinet_pkg_err(
                pkg->pid,
                strlen(err_msg),
                CPROTO_ERR_INSERT,
                err_msg);

        if (package != NULL)
        {
            /* ignore result code, signal can be raised */
            sirinet_pkg_send(client, package);

            /* free package*/
            free(package);
        }

        /* free packer */
        for (size_t n = 0; n < siridb->pools->len; n++)
        {
            qp_packer_free(packer[n]);
        }
        break;
    case ERR_MEM_ALLOC:
        break;
    default:
        siridb_insert_init(
                pkg->pid,
                client,
                (size_t) rc,
                siridb->pools->len,
                packer,
                siridb->flags & SIRIDB_FLAG_REINDEXING);
        break;
    }

    /* free qp_object */
    qp_object_free(qp_obj);

    /* free unpacker */
    qp_unpacker_free(unpacker);
}

static void on_ping(uv_stream_t * client, sirinet_pkg_t * pkg)
{
    sirinet_pkg_t * package;
    package = sirinet_pkg_new(pkg->pid, 0, CPROTO_RES_ACK, NULL);

    if (package != NULL)
    {
        /* ignore result code, signal can be raised */
        sirinet_pkg_send(client, package);
        free(package);
    }
}

static void on_info(uv_stream_t * client, sirinet_pkg_t * pkg)
{
    qp_packer_t * packer = sirinet_packer_new(128);
    if (packer != NULL)
    {
        qp_add_type(packer, QP_ARRAY_OPEN);
        qp_add_string(packer, SIRIDB_VERSION);
        qp_add_type(packer, QP_ARRAY_OPEN);

        if (!llist_walk(
                siri.siridb_list,
                (llist_cb) CLSERVER_on_info_cb,
                packer))
        {
            sirinet_pkg_t * package = sirinet_packer2pkg(
                    packer,
                    pkg->pid,
                    CPROTO_RES_INFO);

            /* ignore result code, signal can be raised */
            sirinet_pkg_send(client, package);

            free(package);
        }
        else
        {
            qp_packer_free(packer);
        }
    }
}

/*
 * This function can raise a SIGNAL.
 */
static void on_loaddb(uv_stream_t * client, sirinet_pkg_t * pkg)
{
    qp_unpacker_t * unpacker = qp_unpacker_new(pkg->data, pkg->len);
    if (unpacker != NULL)
    {
        qp_obj_t * qp_dbpath = qp_object_new();
        if (qp_dbpath != NULL)
        {
            if (qp_next(unpacker, qp_dbpath) == QP_RAW)
            {
                char * dbpath = strndup(qp_dbpath->via->raw, qp_dbpath->len);
                if (dbpath == NULL)
                {
                    ERR_ALLOC
                }
                else
                {
                    siridb_t * siridb = siridb_new(dbpath, LOCK_QUIT_IF_EXIST);
                    if (siridb != NULL)
                    {
                        siridb->server->flags |= SERVER_FLAG_RUNNING;
                    }
                    sirinet_pkg_t * package = sirinet_pkg_new(
                            pkg->pid,
                            0,
                            (siridb == NULL) ?
                                    CPROTO_ERR_LOADING_DB : CPROTO_RES_ACK,
                            NULL);
                    if (package != NULL)
                    {
                        sirinet_pkg_send(client, package);
                        free(package);
                    }
                    free(dbpath);
                }
            }
            else
            {
                log_error("Incorrect package received: 'on_loaddb'");
            }
            qp_object_free(qp_dbpath);
        }
        qp_unpacker_free(unpacker);
    }
}

/*
 * This function can raise a SIGNAL.
 */
static void on_reqfile(
        uv_stream_t * client,
        sirinet_pkg_t * pkg,
        sirinet_clserver_getfile getfile)
{
    CHECK_SIRIDB(ssocket)

    siridb_t * siridb = ssocket->siridb;
    sirinet_pkg_t * package = NULL;
    char err_msg[SIRIDB_MAX_SIZE_ERR_MSG];

    if (siridb->server->flags != SERVER_FLAG_RUNNING)
    {
        snprintf(err_msg,
                SIRIDB_MAX_SIZE_ERR_MSG,
                "Error getting file because server '%s' having status %d",
                siridb->server->name,
                siridb->server->flags);
        package = sirinet_pkg_err(
                pkg->pid,
                strlen(err_msg),
                CPROTO_ERR_SERVER,
                err_msg);
    }
    else if (!siridb_user_check_access(
            (siridb_user_t *) ssocket->origin,
            SIRIDB_ACCESS_PROFILE_FULL,
            err_msg))
    {
        package = sirinet_pkg_err(
                pkg->pid,
                strlen(err_msg),
                CPROTO_ERR_USER_ACCESS,
                err_msg);
    }
    else
    {
        char * content = NULL;
        ssize_t size = getfile(&content, siridb);
        package = (content == NULL) ?
            sirinet_pkg_new(
                    pkg->pid,
                    0,
                    CPROTO_ERR_FILE,
                    NULL) :
            sirinet_pkg_new(
                    pkg->pid,
                    size,
                    CPROTO_RES_FILE,
                    content);
        free(content);
    }

    if (package != NULL)
    {
        sirinet_pkg_send(client, package);
        free(package);
    }
}

/*
 * This function can raise a SIGNAL.
 */
static void on_register_server(uv_stream_t * client, sirinet_pkg_t * pkg)
{
    CHECK_SIRIDB(ssocket)

    siridb_t * siridb = ssocket->siridb;
    sirinet_pkg_t * package = NULL;
    siridb_server_t * new_server = NULL;
    char err_msg[SIRIDB_MAX_SIZE_ERR_MSG];
    slist_t * servers = NULL;


    if (siridb->server->flags != SERVER_FLAG_RUNNING)
    {
        snprintf(err_msg,
                SIRIDB_MAX_SIZE_ERR_MSG,
                "Error getting file because server '%s' having status %d",
                siridb->server->name,
                siridb->server->flags);
        package = sirinet_pkg_err(
                pkg->pid,
                strlen(err_msg),
                CPROTO_ERR_SERVER,
                err_msg);
    }
    else if (!siridb_user_check_access(
            (siridb_user_t *) ssocket->origin,
            SIRIDB_ACCESS_PROFILE_FULL,
            err_msg))
    {
        package = sirinet_pkg_err(
                pkg->pid,
                strlen(err_msg),
                CPROTO_ERR_USER_ACCESS,
                err_msg);
    }
    else if (!siridb_servers_available(siridb))
    {
        sprintf(err_msg, "At least one server is not online or busy");
        package = sirinet_pkg_err(
                pkg->pid,
                strlen(err_msg),
                CPROTO_ERR_SERVER,
                err_msg);
    }
    else
    {
        /* make a copy of the current servers */
        servers = siridb_servers_other2slist(siridb);

        log_info("Register a new server");
        new_server = siridb_server_register(siridb, pkg->data, pkg->len);

        pkg->tp = BPROTO_REGISTER_SERVER;

        if (new_server == NULL)
        {
            /* a signal might be raised */
            package = sirinet_pkg_new(pkg->pid, 0, CPROTO_ERR, NULL);
        }
    }

    if (package != NULL)
    {
        sirinet_pkg_send(client, package);
        free(package);
    }
    else if (new_server != NULL)
    {
        siridb_server_reg_t * server_reg =
                (siridb_server_reg_t *) malloc(sizeof(siridb_server_reg_t));

        if (server_reg == NULL)
        {
            ERR_ALLOC
        }
        else
        {
            /* save the client PID so we can respond to the client */
            server_reg->pid = pkg->pid;
            server_reg->client = client;

            pkg->tp = BPROTO_REGISTER_SERVER;

            /* make sure to unlock the client in the callback */
            sirinet_socket_lock(client);

            if (servers != NULL)
            {
                siridb_servers_send_pkg(
                        servers,
                        pkg,
                        0,
                        (sirinet_promises_cb)
                                CLSERVER_on_register_server_response,
                        server_reg);
            }
        }
    }

    /* free the servers or NULL */
    slist_free(servers);
}

/*
 * Typedef: llist_cb
 * Returns 0 if successful or -1 and a SIGNAL is raised if not.
 */
static int CLSERVER_on_info_cb(siridb_t * siridb, qp_packer_t * packer)
{
    return qp_add_string(packer, siridb->dbname);
}

/*
 * Typedef: sirinet_promises_cb
 */
static void CLSERVER_on_register_server_response(
        slist_t * promises,
        siridb_server_reg_t * server_reg)
{
    if (promises != NULL)
    {
        sirinet_pkg_t * pkg;
        sirinet_promise_t * promise;
        size_t err_count = 0;
        char err_msg[SIRIDB_MAX_SIZE_ERR_MSG];

        for (size_t i = 0; i < promises->len; i++)
        {
            promise = promises->data[i];
            if (promise == NULL)
            {
                err_count++;
                snprintf(err_msg,
                        SIRIDB_MAX_SIZE_ERR_MSG,
                        "Error occurred while registering the new server on "
                        "at least one server");
            }
            else
            {
                pkg = promise->data;

                if (pkg == NULL || pkg->tp != BPROTO_ACK_REGISTER_SERVER)
                {
                    err_count++;
                    snprintf(err_msg,
                            SIRIDB_MAX_SIZE_ERR_MSG,
                            "Error occurred while registering the new server "
                            "on at least '%s'", promise->server->name);
                }

                /* make sure we free the promise and data */
                free(promise->data);
                free(promise);
            }
        }

        sirinet_pkg_t * package = (err_count) ?
                sirinet_pkg_err(
                        server_reg->pid,
                        strlen(err_msg),
                        CPROTO_ERR_MSG,
                        err_msg) :
                sirinet_pkg_new(
                        server_reg->pid,
                        0,
                        CPROTO_RES_ACK,
                        NULL);

        if (package != NULL)
        {
            sirinet_pkg_send(server_reg->client, package);
            free(package);
        }
    }

    /* unlock the client */
    sirinet_socket_unlock(server_reg->client);

    /* free server register object */
    free(server_reg);
}
