/*
 * Copyright (c) 2011, Jason Ish
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "server.h"
#include "confile.h"

/** handler */
#include "handler.h"

/**
 * Set a socket to non-blocking mode.
 */
int
setnonblock(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return flags;

    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0)
        return -1;

        return 0;
}

void
on_handler(struct bufferq *bufferq)
{
    rpc_handler(bufferq);
    return;
}

/**
 * This function will be called by libevent when the client socket is
 * ready for reading.
 */
void
on_read(int fd, short ev, void *arg)
{
    struct client *client = (struct client *)arg;
    struct bufferq *bufferq;
    char *req_buf;
    int body_len = 0;
    int len = 0;

    zlog_debug(zc, "fd: %d", fd);

    /* Because we are event based and need to be told when we can
     * write, we have to malloc the read buffer and put it on the
     * clients write queue. */
    req_buf = malloc(BUFLEN);
    if (req_buf == NULL) {
        err(1, "malloc failed for request buffer.");
    }

    /** 读协议头 */
    len = read(fd, &body_len, PROTOCOL_HEADER_LEN);
    if (PROTOCOL_HEADER_LEN != len || body_len > BUFLEN) {
        zlog_warn(zc, "Protocol header has something wrong. read len: %d, body_len: %d\n", len, body_len);

        goto READ_EXCEPTION;
    }

    zlog_debug(zc, "request.body_len: %d", body_len);

    len = read(fd, req_buf, body_len);
    if (len == 0) {
        /* Client disconnected, remove the read event and the
         * free the client structure. */
        zlog_info(zc, "Client disconnected.\n");

        goto READ_EXCEPTION;
    }
    else if (len < 0) {
        /* Some other error occurred, close the socket, remove
         * the event and free the client structure. */
        zlog_error(zc, "Socket failure, disconnecting client: %s",
            strerror(errno));

        goto READ_EXCEPTION;
    }

    req_buf[body_len] = '\0';   /** 手工将请求的字符串结束 */
    zlog_debug(zc, "request-json: %s", req_buf);

    /* We can't just write the buffer back as we need to be told
     * when we can write by libevent.  Put the buffer on the
     * client's write queue and schedule a write event. */
    bufferq = calloc(1, sizeof(*bufferq));
    if (bufferq == NULL) {
        err(1, "malloc faild for bufferq.");
    }

    /** TODO 解析和创建 json 数据时需要容错 */
    bufferq->request.buf  = req_buf;
    bufferq->request.json = cJSON_Parse(req_buf);

    bufferq->response.buf       = NULL;
    bufferq->response.body_len  = 0;
    bufferq->response.offset    = 0;
    bufferq->response.json      = cJSON_CreateObject();

    on_handler(bufferq);

    TAILQ_INSERT_TAIL(&client->writeq, bufferq, entries);

    /* Since we now have data that needs to be written back to the
     * client, add a write event. */
    event_add(&client->ev_write, NULL);
    return;

READ_EXCEPTION:
    close(fd);

    event_del(&client->ev_read);
    free(client);

    free(req_buf);

    return;
}

/**
 * This function will be called by libevent when the client socket is
 * ready for writing.
 */
void
on_write(int fd, short ev, void *arg)
{
    struct client *client = (struct client *)arg;
    struct bufferq *bufferq;
    int len;

    /* Pull the first item off of the write queue. We probably
     * should never see an empty write queue, but make sure the
     * item returned is not NULL. */
    bufferq = TAILQ_FIRST(&client->writeq);
    if (bufferq == NULL)
        return;

    /** 写头协议 */
    if (0 == bufferq->response.offset) {
        write(fd, &(bufferq->response.body_len), PROTOCOL_HEADER_LEN);
    }

    /* Write the buffer.  A portion of the buffer may have been
     * written in a previous write, so only write the remaining
     * bytes. */
    len = bufferq->response.body_len - bufferq->response.offset;
    len = write(fd, bufferq->response.buf + bufferq->response.offset,
                    bufferq->response.body_len - bufferq->response.offset);
    if (len == -1) {
        if (errno == EINTR || errno == EAGAIN) {
            /* The write was interrupted by a signal or we
             * were not able to write any data to it,
             * reschedule and return. */
            event_add(&client->ev_write, NULL);
            return;
        }
        else {
            /* Some other socket error occurred, exit. */
            err(1, "write");
        }
    }
    else if ((bufferq->response.offset + len) < bufferq->response.body_len) {
        /* Not all the data was written, update the offset and
         * reschedule the write event. */
        bufferq->response.offset += len;
        event_add(&client->ev_write, NULL);
        return;
    }

    /* The data was completely written, remove the buffer from the
     * write queue. */
    TAILQ_REMOVE(&client->writeq, bufferq, entries);

    free(bufferq->request.buf);
    free(bufferq->response.buf);

    if (NULL != bufferq->request.json) {
        cJSON_Delete(bufferq->request.json);
    }
    if (NULL != bufferq->response.json) {
        cJSON_Delete(bufferq->response.json);
    }

    free(bufferq);
}

/**
 * This function will be called by libevent when there is a connection
 * ready to be accepted.
 */
void
on_accept(int fd, short ev, void *arg)
{
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct client *client;

    /* Accept the new connection. */
    client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd == -1) {
        warn("accept failed");
        return;
    }

    /* Set the client socket to non-blocking mode. */
    if (setnonblock(client_fd) < 0)
        warn("failed to set client socket non-blocking");

    /* We've accepted a new client, allocate a client object to
     * maintain the state of this client. */
    client = calloc(1, sizeof(*client));
    if (client == NULL)
        err(1, "malloc failed");

    /* Setup the read event, libevent will call on_read() whenever
     * the clients socket becomes read ready.  We also make the
     * read event persistent so we don't have to re-add after each
     * read. */
    event_set(&client->ev_read, client_fd, EV_READ|EV_PERSIST, on_read,
        client);

    /* Setting up the event does not activate, add the event so it
     * becomes active. */
    event_add(&client->ev_read, NULL);

    /* Create the write event, but don't add it until we have
     * something to write. */
    event_set(&client->ev_write, client_fd, EV_WRITE, on_write, client);

    /* Initialize the clients write queue. */
    TAILQ_INIT(&client->writeq);

    zlog_debug(zc, "Accepted connection from %s\n",
        inet_ntoa(client_addr.sin_addr));
}

/**
 * global variables
 * */
zlog_category_t *zc;

int
main(int argc, char **argv)
{
    int rc;

    rc = zlog_init("conf/zlog.conf");
    if (rc) {
        fprintf(stderr, "init failed\n");
        return -1;
    }

    zc = zlog_get_category("main_cat");
    if (!zc) {
        fprintf(stderr, "get cat fail\n");
        zlog_fini();
        return -2;
    }
    zlog_info(zc, "程序初始化");

    int listen_fd;
    struct sockaddr_in listen_addr;
    int reuseaddr_on = 1;

    /* The socket accept event. */
    struct event ev_accept;

    /* Initialize libevent. */
    zlog_debug(zc, "初始化 libevent");
    event_init();

    /* Create our listening socket. This is largely boiler plate
     * code that I'll abstract away in the future. */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        err(1, "listen failed");
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_on,
        sizeof(reuseaddr_on)) == -1) {
        err(1, "setsockopt failed");
    }
    zlog_debug(zc, "创建 socket 成功");

    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&listen_addr,
        sizeof(listen_addr)) < 0)
        err(1, "bind failed");
    if (listen(listen_fd, 5) < 0)
        err(1, "listen failed");
    zlog_debug(zc, "bind 端口成功");

    /* Set the socket to non-blocking, this is essential in event
     * based programming with libevent. */
    if (setnonblock(listen_fd) < 0)
        err(1, "failed to set server socket to non-blocking");

    /* We now have a listening socket, we create a read event to
     * be notified when a client connects. */
    event_set(&ev_accept, listen_fd, EV_READ|EV_PERSIST, on_accept, NULL);
    event_add(&ev_accept, NULL);
    zlog_debug(zc, "libevent 初始化完成");

    /* Start the libevent event loop. */
    event_dispatch();

    /** 销毁日志句柄 */
    zlog_fini();

    return 0;
}
