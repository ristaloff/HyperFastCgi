#include <stdlib.h>
#include <glib.h>
#include <pthread.h>
#include <string.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include "fcgi.h"
#include "libev.h"
#include "mono-bridge.h"
#include "fcgi-transport.h"

#define GET_HASH(fd,reqid) (((guint64)(reqid)) << 32) ^ (fd)

typedef struct {
    guint64 hash;
    int fd;
    guint16 requestId;
    int request_num;
    FCGI_Header* header;
    unsigned char* body;
    gboolean stdout_sent;
    gboolean keep_alive;
    HostInfo *host_info;
    gchar *hostname;
    int port;
    gchar *vpath;
    GStringChunk *chunks;
    GArray *key_value_pairs;
} Request;

typedef struct {
    gchar *name;
    int nlen;
    gchar *value;
    int vlen;
    gboolean is_header;
} KeyValuePair;

static GHashTable* requests;
static pthread_mutex_t requests_lock;

static int request_num = 0;
static int finalized = 0;

static gboolean
parse_params(Request *req, FCGI_Header* header, guint8* data);


void
transport_init()
{
    pthread_mutex_init (&requests_lock, NULL);
    requests = g_hash_table_new (g_int64_hash, g_int64_equal);
}

void
transport_finalize()
{
    finalized = 1;
    pthread_mutex_destroy (&requests_lock);
    g_hash_table_destroy (requests);
}

static const char* Header="HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: 20\r\n\r\n";
static const char* Response="<p>Hello, world!</p>";
static const char* error404 = "HTTP/1.0 404 Not Found\r\n" \
			           "Connection: close\r\n\r\n" \
			           "<html><head><title>404 Not Found</title></head>\r\n" \
			           "<body><h1>Not Found</h1>The requested URL %s was not found on this " \
			           "server.<p>\r\n</body></html>\r\n";


static void process_internal (Request *req, FCGI_Header *header, guint8 *body, int len)
{
//    INFO_OUT("%s","process internal");
    //process(host, req->hash, req->request_num);
    add_body_part(req->host_info, req->hash, req->request_num, body, len, len == 0);

//    send_output(req->hash, req->request_num, (guint8 *)Header, strlen(Header));
//    send_output(req->hash, req->request_num, (guint8 *)Response, strlen(Response));
//    end_request(req->hash, req->request_num, 200, FCGI_REQUEST_COMPLETE);
}

void
process_record(int fd, FCGI_Header* header, guint8* body)
{
	INFO_OUT("process_record, fd=%i\r\n", fd);
    Request *req = NULL;
    guint64 id = GET_HASH(fd,fcgi_get_request_id(header));

    if (finalized) return;

    //key not found. if header->type is FCGI_BEGIN_REQUEST
    //than create new request, otherwise skip request due to FastCGI spec
    pthread_mutex_lock (&requests_lock);
    req = g_hash_table_lookup (requests, &id);

    if (!req) {
        if (header->type == FCGI_BEGIN_REQUEST) {
            FCGI_BeginRequestBody *begin_body = (FCGI_BeginRequestBody *)body;

            req = g_new (Request, 1);
            req->hash = id;
            req->request_num = ++request_num;

            g_hash_table_insert (requests, &req->hash, req);
            pthread_mutex_unlock (&requests_lock);

            req->fd = fd;
            req->requestId = fcgi_get_request_id(header);
            req->header = header;
            req->body = body;
            req->keep_alive = begin_body->flags & FCGI_KEEP_CONN;
            req->stdout_sent = FALSE;
            req->hostname = NULL;
            req->port = -1;
            req->vpath = NULL;

            //TODO: host_info must be set in parse_params
            req->host_info = find_host_by_path(NULL, -1, NULL);

            //if host is not single, preallocate space for server variables
            if (!req->host_info) {
                req->chunks = g_string_chunk_new(4096);
                req->key_value_pairs = g_array_sized_new(FALSE, FALSE, sizeof(KeyValuePair), 128);
            } else {
                //host is single, so we can create request now
                create_request (req->host_info, req->hash, req->request_num);
            }

            return;
        }
    }
    pthread_mutex_unlock (&requests_lock);

    if (req) {
        switch(header->type)
        {
            case FCGI_BEGIN_REQUEST:
                //TODO: assert should not be reached
                INFO_OUT("FCGI_BEGIN_REQUEST\r\n");
                break;
            case FCGI_ABORT_REQUEST:
            	INFO_OUT("FCGI_ABORT_REQUEST\r\n");
                //TODO: remove from hash, send abort to web server
                break;
            case FCGI_PARAMS:
            	INFO_OUT("FCGI_PARAMS\r\n");
                parse_params (req, header, body);
                break;
            case FCGI_STDIN:
                //TODO: read until the end
                INFO_OUT("FCGI_STDIN\r\n");
                process_internal (req, header, body, fcgi_get_content_len(header));
                break;
            case FCGI_DATA:
            	INFO_OUT("FCGI_DATA\r\n");
                //TODO: nothing?
                break;
            case FCGI_GET_VALUES:
            	INFO_OUT("FCGI_GET_VALUES\r\n");
                //currently there are no server-side settings (values)
                break;
            default:
            	INFO_OUT("default\r\n");
                break;
        }
    }
}

static int errorCount = 0; //debug-helper

static void
send_record (cmdsocket* sock, guint8 record_type, guint16 requestId, guint8* data, int offset, int len)
{
    INFO_OUT("Send record reqId=%i, fd=%i, offset=%i, len=%i, errorCount=%i.\r\n", requestId, sock->fd, offset, len, errorCount);
    FCGI_Header header = {
        .version = FCGI_VERSION_1,
        .type = record_type,
        .paddingLength = 0,
        .reserved = 0,
    };

    if(!sock || !sock->buf_event){
        ERROR_OUT("sock or sock->buf_event is null\r\n");
        errorCount++;
        return;
    }

    fcgi_set_request_id (&header, requestId);
    fcgi_set_content_len (&header, len);
    printf("EnabledBufferEvents:");
    printf("no-locking=%i->", sock->buf_event->enabled);
    //printf("with-locking=%i->", bufferevent_get_enabled(sock->buf_event));   //crashes here once every 10000 times or so.
//    evbuffer_lock(output);
//    evbuffer_add (output, &header, FCGI_HEADER_SIZE);
//    evbuffer_add (output, data + offset, len);

	if(!sock || !sock->buf_event){
        ERROR_OUT("Check 2. sock or sock->buf_event is null\r\n");
        errorCount++;
        return;
    }

    printf("evbuffer:");
    struct evbuffer *output = bufferevent_get_output(sock->buf_event);
    //struct evbuffer *input = bufferevent_get_input(sock->buf_event);
    //evbuffer_unlock(output);

    if(!output){
        ERROR_OUT("Output is null\r\n");
        errorCount++;
        return;
    }
//    if(!input){
//        ERROR_OUT("Input is null\r\n");
//       errorCount++;
//        return;
//    }
    printf("address=%p->", &output);

    if(sock->buf_event->enabled == 4){
    	ERROR_OUT("sock->buf_event->enabled == 4\r\n");
       	errorCount++;
    	return;
    }
    printf("ErrorCount=%i->", errorCount);
    //size_t evbuffer_len = evbuffer_get_length(input); 
    size_t evbuffer_len = evbuffer_get_length(output); //crashes...
    printf("evbuffer-len=");
    printf("%zu->",evbuffer_len);
    //printf("lenght=%zu.->",output->total_len);

    //printf("Locking->");
    //bufferevent_lock (sock->buf_event); //crash here
    if(evbuffer_len && evbuffer_len > 0){
      	printf("Write1:");
   		int write1success = bufferevent_write_buffer(sock->buf_event, output);
   		printf("%i->", write1success);
   	}
   	else{
   		printf("skip->");
   	}
   
    printf("Write2:");
    int write2Success = bufferevent_write(sock->buf_event, &header, FCGI_HEADER_SIZE);
    printf("%i->Write3:", write2Success);
    int write3success = bufferevent_write(sock->buf_event, data + offset, len);
    printf("%i->", write3success);
     //printf("Unlocking->");
    //bufferevent_unlock (sock->buf_event);

    printf("Sent!\n\r");
//    bufferevent_lock (sock->buf_event);
//    bufferevent_write_buffer(sock->buf_event, sock->buffer);
//    bufferevent_unlock (sock->buf_event);
}

static void
send_stream_data (cmdsocket* sock, guint8 record_type, guint16 requestId, guint8* data, int len)
{
    if (len < FCGI_MAX_BODY_SIZE)
        send_record (sock, record_type, requestId, data, 0, len);
    else {
		INFO_OUT("request n=%i larger than FCGI_MAX_BODY_SIZE\r\n", request_num);
        int index=0;
        while (index < len) {
            int chunk_len = (len - index < FCGI_SUGGESTED_BODY_SIZE)
                            ? (len - index)
                            : FCGI_SUGGESTED_BODY_SIZE;
            send_record (sock, record_type, requestId, data, index, chunk_len);

            index += chunk_len;
        }
    }
}

void
send_output (guint64 requestId, int request_num, guint8* data, int len)
{
	INFO_OUT("SendOutput for request n=%i start\r\n", request_num);
    if (finalized) return;
    pthread_mutex_lock (&requests_lock);
    Request* req=(Request *)g_hash_table_lookup (requests, &requestId);
    pthread_mutex_unlock (&requests_lock);
	//INFO_OUT("SendOutput for request n=%i unlocked\r\n", request_num);
    if (req && req->request_num == request_num) {
        cmdsocket* sock = find_cmdsocket (req->fd);
        if (sock != NULL) {
            send_stream_data (sock, FCGI_STDOUT, req->requestId, data, len);
        }
    } else {
        INFO_OUT ("can't find request n=%i\r\n", request_num);
    }
	//INFO_OUT("SendOutput for request n=%i done\r\n", request_num);
}

void
end_request (guint64 requestId, int request_num, int app_status, int protocol_status)
{
	//INFO_OUT("Ending request n=%i ...\r\n", request_num);
    FCGI_EndRequestBody body = {
        .reserved1 = 0,
        .reserved2 = 0,
        .reserved3 = 0
    };

    if (finalized) {
        INFO_OUT("Request n=%i. Is finalized.\r\n", request_num);
        return;
    }

    pthread_mutex_lock (&requests_lock);
    Request *req=(Request *)g_hash_table_lookup (requests, &requestId);

    if(req){
    	INFO_OUT("Ending Request n=%i, addr=%p.\r\n", request_num, &req);
    }
    else{
    	INFO_OUT("Ending Request n=%i, Request pointer is null!.\r\n", request_num);
    }
    
    if (req && req->request_num == request_num) {
        g_hash_table_remove(requests, &requestId);
        pthread_mutex_unlock (&requests_lock);
        cmdsocket* sock = find_cmdsocket (req->fd);
        if (sock != NULL) {
            fcgi_set_app_status (&body, app_status);
            body.protocolStatus=protocol_status;
            send_record (sock, FCGI_END_REQUEST, req->requestId, (guint8 *)&body, 0, sizeof (body));

            INFO_OUT("Request n=%i. Last record sent.\r\n", request_num);
            //flush and disconnect cmdsocket if KEEP_ALIVE is false

            if (!req->keep_alive) {
                INFO_OUT("Request n=%i. Keep alive false. Flush cmd_socket...\r\n", request_num);
                flush_cmdsocket(sock);
            }
        }

        if(req){
    		INFO_OUT("Calling g_free(req) n=%i, addr=%p.\r\n", request_num, &req);
	    }
	    else{
	    	INFO_OUT("Calling g_free(req) n=%i, Request pointer is null!.\r\n", request_num);
	    }
	    g_free (req);
        //if(req
        //INFO_OUT("Request n=%i, addr=%p. Calling g_free(req)\r\n", request_num, &req);

    }
    else {
        INFO_OUT ("can't find request n=%i\r\n",request_num);
        pthread_mutex_unlock (&requests_lock);
    }
	INFO_OUT("Request n=%i ended\r\n", request_num);
}

static gboolean
parse_params(Request *req, FCGI_Header *header, guint8 *data)
{
    int data_len = fcgi_get_content_len(header);
    int offset = 0;
    int nlen, vlen;
    gchar *name, *value;

    while (offset < data_len) {
        nlen = data[offset++];

        if (nlen >= 0x80) {
            nlen = ((0x7F & nlen) << 24)
            | (*(data + offset) << 16)
            | (*(data + offset + 1) << 8)
            | *(data + offset + 2);
            offset += 3;
        }

        vlen = data [offset++];

        if (vlen >= 0x80) {
            vlen = ((0x7F & vlen) << 24)
            | (*(data + offset) << 16)
            | (*(data + offset + 1) << 8)
            | *(data + offset + 2);

            offset += 3;
        }

        if (offset + nlen + vlen > data_len){
            INFO_OUT("ArgumentOutOfRangeException: offset\r\n");
        }

        name = (gchar *)(data + offset);
        offset += nlen;
        value = (gchar *)(data + offset);
        offset += vlen;

        //params can be server vars or http headers.
        //If param starts from "HTTP_" then it is HTTP header and needs to be reformatted
        if (nlen > 5 && !memcmp(name,"HTTP_",5)) {
            //reformat HTTP header to common name
            //skip "HTTP_" prefix
            name += 5;
            nlen -= 5;
            int i = 0;
            gboolean upper_case = TRUE;

            while (i < nlen) {
                if (name [i] == '_') {
                    name [i] = '-';
                    upper_case = TRUE;
                } else {
                    name [i] = upper_case ? name [i] : g_ascii_tolower (name [i]);
                    upper_case = FALSE;
                }
                i++;
            }
            //call add header function
            if (req->host_info)
                add_header(req->host_info, req->hash, req->request_num, name, nlen, value, vlen);
            else {
                //save header to temporary place. This code should not be run
                //if server variables go first
                KeyValuePair pair;
                pair.name = g_string_chunk_insert_len(req->chunks, name, nlen);
                pair.nlen = nlen;
                pair.value = g_string_chunk_insert_len(req->chunks, value, vlen);
                pair.vlen = vlen;
                pair.is_header = TRUE;
                g_array_append_val(req->key_value_pairs, pair);
            }
        }
        else { /* server variable */
            //TODO: call function to add server param
            if (req->host_info)
                add_server_variable(req->host_info, req->hash, req->request_num, name, nlen, value, vlen);
            else {
                //add server variable to the temporary place
                KeyValuePair pair;
                pair.name = g_string_chunk_insert_len(req->chunks, name, nlen);
                pair.nlen = nlen;
                pair.value = g_string_chunk_insert_len(req->chunks, value, vlen);
                pair.vlen = vlen;
                pair.is_header = FALSE;
                g_array_append_val(req->key_value_pairs, pair);

                //we need to get host, port and vpath from server variables
                //when we'll get them all, we can find a route to appropriate host
                //TODO: we can reference chunks instead of using strndup
                if (!req->hostname && nlen == 11 && (memcmp(name,"SERVER_NAME",11) == 0)) {
                        req->hostname = pair.value;
 //                       INFO_OUT("VHost=%s\n", req->hostname);
                }

                if (req->port == -1 && nlen == 11 && (memcmp(name,"SERVER_PORT",11) == 0)) {
                        req->port = atoi(pair.value);
  //                      INFO_OUT("VPort=%i\n", req->port);
                }

                if (!req->vpath && nlen == 11 && (memcmp(name,"SCRIPT_NAME",11) == 0)) {
                    req->vpath = pair.value;
   //                 INFO_OUT("VPath=%s\n", req->vpath);
                }
   //             INFO_OUT("name=%s value=%s\n",pair.name, pair.value);

                //check, that we've got all server variables we're needed
                if (req->hostname && req->port != -1 && req->vpath) {
                    //now we can find host by path
                    INFO_OUT("host=%s port=%i vpath=%s\n", req->hostname, req->port, req->vpath);
                    req->host_info = find_host_by_path(req->hostname, req->port, req->vpath);

                    //if *.webapp configuration is OK, we'll found a host and
                    //can send all server variables and headers, we've saved to
                    //temporary place before
                    if (req->host_info) {
                        //create request on the bridge
                        create_request (req->host_info, req->hash, req->request_num);

                        //send all variables to host
                        int i;
                        for (i=0; i<req->key_value_pairs->len; i++) {
                            KeyValuePair pair = g_array_index(req->key_value_pairs,KeyValuePair, i);
                            if (pair.is_header) {
                                add_header(req->host_info, req->hash, req->request_num, pair.name, pair.nlen, pair.value, pair.vlen);
                            } else {
                                add_server_variable(req->host_info, req->hash, req->request_num, pair.name, pair.nlen, pair.value, pair.vlen);
                            }
                        }
                        //free temporary resources
                        g_array_free(req->key_value_pairs, TRUE);
                        g_string_chunk_free(req->chunks);
                    } else {
                        //something wrong with configuration... we can't find the host
                        ERROR_OUT("Can't find app! HOST='%s' port=%i path='%s'\n",req->hostname, req->port, req->vpath);
                        //send 404 not found
                        gchar *err = g_strdup_printf(error404, req->vpath);
                        send_output(req->hash, req->request_num, (guint8 *)err, strlen(err));
                        g_free(err);
                        //free temporary resources
                        g_array_free(req->key_value_pairs, TRUE);
                        g_string_chunk_free(req->chunks);
                        //end request
                        end_request(req->hash, req->request_num, 0, FCGI_REQUEST_COMPLETE);
                        return FALSE;
                    }

                }

            }
        }
    }

    //when we receive FCGI_PARAMS with content_len == 0
    //this means, that server passed all the data.
    return data_len == 0;
}
