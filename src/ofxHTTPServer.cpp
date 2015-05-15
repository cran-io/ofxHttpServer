/*
 * ofxHTTPServer.cpp
 *
 *  Created on: 16-may-2009
 *      Author: art
 */

#include "ofxHTTPServer.h"
#include <cstring>
#include <fstream>
#include <map>

using namespace std;

ofxHTTPServer ofxHTTPServer::instance;

// Helper functions and structures only used internally by the server
//------------------------------------------------------
static const int GET = 0;
static const int POST = 1;
static const unsigned POSTBUFFERSIZE = 4096;
static const char* CONTENT_TYPE = "Content-Type";

class connection_info{
	static int id;
public:
	connection_info(){
		conn_id = ++id;
	}

	map<string,string> fields;
	map<string,FILE*> file_fields;
	map<string,string> file_to_path_index;
	map<string,string> file_to_key_index;
	int connectiontype;
	bool connection_complete;
	struct MHD_PostProcessor *postprocessor;
	int conn_id;
	char new_content_type[1024];

};

int connection_info::id=0;


// private methods

int ofxHTTPServer::print_out_key (void *cls, enum MHD_ValueKind kind, const char *key, const char *value)
{
  ofLogVerbose ("ofxHttpServer") << ofVAArgsToString("%s = %s\n", key, value);
  return MHD_YES;
}


int ofxHTTPServer::get_get_parameters (void *cls, enum MHD_ValueKind kind, const char *key, const char *value)
{
	connection_info *con_info = (connection_info*) cls;
	if(key!=NULL && value!=NULL)
	con_info->fields[key] = value;
	return MHD_YES;
}

void ofxHTTPServer::request_completed (void *cls, struct MHD_Connection *connection, void **con_cls,
                        enum MHD_RequestTerminationCode toe)
{
  connection_info *con_info = (connection_info*) *con_cls;


  if (NULL == con_info){
	  ofLogWarning("ofxHttpServer") << "request completed NULL connection";
	  return;
  }

  if (con_info->connectiontype == POST){
      MHD_destroy_post_processor (con_info->postprocessor);
  }



  delete con_info;
  *con_cls = NULL;

  instance.maxActiveClientsMutex.lock();
  instance.numClients--;
  instance.maxActiveClientsCondition.signal();
  instance.maxActiveClientsMutex.unlock();
}

int ofxHTTPServer::send_page (struct MHD_Connection *connection, long length, const char* page, int status_code, string contentType)
{
  int ret;
  struct MHD_Response *response;


  response = MHD_create_response_from_data (length, (void*) page, MHD_NO, MHD_YES);
  if (!response) return MHD_NO;

  if(contentType!=""){
	  MHD_add_response_header (response,"Content-Type",contentType.c_str());
  }

  ret = MHD_queue_response (connection, status_code, response);
  MHD_destroy_response (response);

  return ret;
}

int ofxHTTPServer::send_redirect (struct MHD_Connection *connection, const char* location, int status_code)
{
  int ret;
  struct MHD_Response *response;


  char data[]="";
  response = MHD_create_response_from_data (0,data, MHD_NO, MHD_YES);
  if (!response) return MHD_NO;

  MHD_add_response_header (response, "Location", location);

  ret = MHD_queue_response (connection, status_code, response);
  MHD_destroy_response (response);

  return ret;
}



// public methods
//------------------------------------------------------
ofxHTTPServer::ofxHTTPServer() {
	maxClients = 100;
	numClients = 0;
	maxActiveClients = 4;
	http_daemon = NULL;
	port = 8888;
	listener = NULL;
}

ofxHTTPServer::~ofxHTTPServer() {
	// TODO Auto-generated destructor stub
}



int ofxHTTPServer::answer_to_connection(void *cls,
			struct MHD_Connection *connection, const char *url,
			const char *method, const char *version, const char *upload_data,
			size_t *upload_data_size, void **con_cls) {

	string strmethod = method;

	connection_info  * con_info;

	// to process post we need several iterations, first we set a connection info structure
	// and return MHD_YES, that will make the server call us again
	if(NULL == *con_cls){
		con_info = new connection_info;

		instance.maxActiveClientsMutex.lock();
		instance.numClients++;
		if(instance.numClients >= instance.maxClients){
			instance.maxActiveClientsMutex.unlock();
			ofFile file503("503.html");
			ofBuffer buf;
			file503 >> buf;
			return send_page(connection, buf.size(), buf.getBinaryBuffer(), MHD_HTTP_SERVICE_UNAVAILABLE);
		}

		if(instance.numClients > instance.maxActiveClients){
			instance.maxActiveClientsCondition.wait(instance.maxActiveClientsMutex);
		}
		instance.maxActiveClientsMutex.unlock();

		// super ugly hack to manage poco multipart post connections as it sets boundary between "" and
		// libmicrohttpd doesn't seem to support that
		string contentType;
		if(MHD_lookup_connection_value(connection, MHD_HEADER_KIND, CONTENT_TYPE)!=NULL)
			contentType = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, CONTENT_TYPE);
		if ( contentType.size()>31 && contentType.substr(0,31) == "multipart/form-data; boundary=\""){
			contentType = "multipart/form-data; boundary="+contentType.substr(31,contentType.size()-32);
			ofLogVerbose("ofxHttpServer") << "changing content type: " << contentType << endl;
			strcpy(con_info->new_content_type,contentType.c_str());
			MHD_set_connection_value(connection,MHD_HEADER_KIND,CONTENT_TYPE,con_info->new_content_type);
		}
		MHD_get_connection_values (connection, MHD_HEADER_KIND, print_out_key, NULL);

		if(strmethod=="GET"){
			con_info->connectiontype = GET;
			MHD_get_connection_values (connection, MHD_GET_ARGUMENT_KIND, get_get_parameters, con_info);
		}else if (strmethod=="POST"){
            ofLogWarning("ofxHttpServer") << "received POST - not supported" << endl;
            delete con_info;
            return MHD_NO;
		}

		*con_cls = (void*) con_info;
		return MHD_YES;
	}else{
		con_info = (connection_info*) *con_cls;
	}


	// second and next iterations
	string strurl = url;
	int ret = MHD_HTTP_SERVICE_UNAVAILABLE;


    // Always call the callback
    ofLogVerbose("ofxHttpServer") << method << " serving from callback: " << url << endl;

    ofxHTTPServerResponse response;
    response.url = strurl;
    const char * referer = MHD_lookup_connection_value(connection,MHD_HEADER_KIND,MHD_HTTP_HEADER_REFERER);
    if(referer){
        response.referer = referer;
    }

    if(strmethod=="GET"){
        response.requestFields = con_info->fields;
        if(instance.listener) instance.listener->getRequest(response);
        //ofNotifyEvent(instance.getEvent,response);
        if(response.errCode>=300 && response.errCode<400){
            ret = send_redirect(connection, response.location.c_str(), response.errCode);
        }else{
            ret = send_page(connection, response.response.size(), response.response.getBinaryBuffer(), response.errCode, response.contentType);
        }
    }else if (strmethod=="POST"){
        if (*upload_data_size != 0){
            ret = MHD_post_process(con_info->postprocessor, upload_data, *upload_data_size);
            *upload_data_size = 0;

        }else{
            ofLogVerbose("ofxHttpServer") << "upload_data_size =  0" << endl;
            response.requestFields = con_info->fields;
            map<string,string>::iterator it_f;
            for(it_f=con_info->fields.begin();it_f!=con_info->fields.end();it_f++){
                ofLogVerbose("ofxHttpServer") << it_f->first << ", " << it_f->second;
            }
            map<string,FILE*>::iterator it;
            for(it=con_info->file_fields.begin();it!=con_info->file_fields.end();it++){
                  if(it->second!=NULL){
                      fflush(it->second);
                      fclose(it->second);
                      response.uploadedFiles[con_info->file_to_key_index[it->first]]=con_info->file_to_path_index[it->first];
                  }
            }
            //ofNotifyEvent(instance.postEvent,response);
            if(instance.listener) instance.listener->postRequest(response);
            if(response.errCode>=300 && response.errCode<400){
                ret = send_redirect(connection, response.location.c_str(), response.errCode);
            }else{
                ret = send_page(connection, response.response.size(), response.response.getBinaryBuffer(), response.errCode, response.contentType);
            }
        }

    }

	return ret;

}


void ofxHTTPServer::start(unsigned _port, bool threaded) {
	port = _port;
	/*if(threaded){
		http_daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY,
				_port, NULL, NULL,
				&answer_to_connection, NULL, MHD_OPTION_NOTIFY_COMPLETED,
	            &request_completed, NULL,
	            MHD_OPTION_THREAD_POOL_SIZE,100,
	            MHD_OPTION_END);
	}else{*/
		http_daemon = MHD_start_daemon(threaded?MHD_USE_THREAD_PER_CONNECTION:MHD_USE_SELECT_INTERNALLY,
				_port, NULL, NULL,
				&answer_to_connection, NULL, MHD_OPTION_NOTIFY_COMPLETED,
	            &request_completed, NULL, MHD_OPTION_END);
	//}
}

void ofxHTTPServer::stop(){
	MHD_stop_daemon(http_daemon);
}

void ofxHTTPServer::setMaxNumberClients(unsigned num_clients){
	maxClients = num_clients;
}

void ofxHTTPServer::setMaxNumberActiveClients(unsigned num_clients){
	maxActiveClients = num_clients;
}

unsigned ofxHTTPServer::getNumberClients(){
	return numClients;
}

void ofxHTTPServer::setListener(ofxHTTPServerListener & _listener){
	listener = &_listener;
}
