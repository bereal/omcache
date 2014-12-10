#include <string.h>
#include <endian.h>
#include <arpa/inet.h>
#include <v8.h>
#include <node.h>
#include <omcache.h>
#include <poll.h>
#include <iostream>
#include <sstream>
#include <list>
#include <map>
#include <uv.h>

using namespace v8;

const uint8_t CMD_GET=0x00;
const uint8_t CMD_SET=0x01;
const uint8_t CMD_INC=0x05;
const uint8_t CMD_DEC=0x06;

/*
  TODO:
  - more requests
  - memory cleanup
  - comments
  - tests
  - refactoring
 */


/************************************************************
 * Prepare data and populate the request
 ************************************************************/
class RequestTemplate {

public:
    RequestTemplate(uint8_t opcode, uint64_t cas);
    RequestTemplate & SetKey(Handle<Value>);
    RequestTemplate & SetData(Handle<Value>);
    RequestTemplate & SetExtra(const void * extra, size_t extralen);
    void Fill(omcache_req_t *request);

private:
    static void CopyUtf8(Handle<Value> data, unsigned char *&dest, size_t  &len);
    uint8_t opcode;
    uint64_t cas;

    unsigned char * key_bytes;
    unsigned char * data_bytes;
    unsigned char * extra_bytes;

    size_t keylen;
    size_t datalen;
    size_t extralen;
};

RequestTemplate::RequestTemplate(uint8_t opcode_, uint64_t cas_): opcode(opcode_), cas(cas_),
                                                                  key_bytes(NULL), data_bytes(NULL), extra_bytes(NULL),
                                                                  keylen(0), datalen(0), extralen(0) {}

void RequestTemplate::CopyUtf8(Handle<Value> data, uint8_t *& dest, size_t &len) {
    Handle<String> str = data->ToString();
    String::Utf8Value utf8(data->ToString());
    len = str->Utf8Length();
    dest = new unsigned char [len];
    memcpy(dest, *utf8, len);
}

RequestTemplate & RequestTemplate::SetKey(Handle<Value> keyval) {
    CopyUtf8(keyval, key_bytes, keylen);
    return *this;
}

RequestTemplate & RequestTemplate::SetData(Handle<Value> dataval) {
    CopyUtf8(dataval, data_bytes, datalen);
    return *this;
}

RequestTemplate & RequestTemplate::SetExtra(const void * extra_bytes, size_t extralen) {
    this->extra_bytes = new unsigned char[extralen];
    memcpy(this->extra_bytes, extra_bytes, extralen);
    this->extralen = extralen;
    return *this;
}

void RequestTemplate::Fill(omcache_req_t *request) {
    memset(request, 0, sizeof(omcache_req_t));
    request->server_index = -1;
    request->header.opcode = this->opcode;
    request->header.keylen = htons(keylen);
    request->header.extlen = extralen;
    request->header.bodylen = htonl(extralen + keylen + datalen);
    request->header.cas = cas;
    request->key = key_bytes;
    request->data = data_bytes;
    request->extra = extra_bytes;
}

/************************************************************
 * Wraps a JS callback and monitors the request status
 ************************************************************/
class Callback {
public:
    Callback(omcache_t *omc): m_omc(omc), m_request_count(1), m_has_timer(false), m_called(false) {};
    bool SendCommand(RequestTemplate &rt, Persistent<Function> callback, int timeout=0);

    // Check results and call the callback if needed
    bool Ping();

    // Check if the request is complete and the callback may be destroyed
    inline bool Done() { return m_called; }
    ~Callback();
private:
    // Handle timeout events
    static void Timeout(uv_timer_t * handle, int);
    void ProcessTimeout(uv_timer_t *handle);

    omcache_t *m_omc;
    omcache_req_t m_request;
    size_t m_request_count;
    uv_timer_t m_timer;
    bool m_has_timer;
    Handle<Function> m_callback;
    bool m_called;
};

Callback::~Callback() {
    if (m_request.key)   delete [] const_cast<unsigned char*>(m_request.key);
    if (m_request.data)  delete [] const_cast<unsigned char*>(m_request.data);
    if (m_request.extra) delete [] const_cast<unsigned char *>(static_cast<const unsigned char*>(m_request.extra));

    if (m_has_timer) {
        uv_timer_stop(&m_timer);
        uv_unref((uv_handle_t*)&m_timer);
    }
}

bool Callback::SendCommand(RequestTemplate &rt, Persistent<Function> callback, int timeout) {
    m_callback = callback; //Persistent<Function>::New(Local<Function>::Cast(callback));
    rt.Fill(&m_request);
    omcache_command(m_omc, &m_request, &m_request_count, NULL, NULL, 0);
    if (timeout > 0) {
        uv_timer_init(uv_default_loop(), &m_timer);
        m_timer.data = this;
        uv_timer_start(&m_timer, Timeout, timeout, 0);
        m_has_timer = true;
    }

    return true;
}

bool Callback::Ping() {
    if (m_called) return true;
    omcache_value_t value;
    memset(&value, 0, sizeof(omcache_value_t));
    size_t value_count=1;

    if (!m_request_count) return true;

    int ret = omcache_io(m_omc, &m_request, &m_request_count, &value, &value_count, 0);
    if (ret == OMCACHE_AGAIN) return false;

    HandleScope scope;

    Handle<Value> argv[2] = {Undefined(), Undefined()};
    Handle<Value> data = Undefined();

    if (value_count && value.data) {
        data = String::New(reinterpret_cast<const char *>(value.data), value.data_len);
    }

    m_called = true;

    argv[(value.status == OMCACHE_OK) ? 1 : 0] = data;
    m_callback->Call(Context::GetCurrent()->Global(), 2, argv);

    scope.Close(Undefined());
    return true;
}

void Callback::Timeout(uv_timer_t *handle, int) {
    Callback * this_ = static_cast<Callback*>(handle->data);
    if (this_->m_called) return; // actually, should not happen
    this_->ProcessTimeout(handle);
}

void Callback::ProcessTimeout(uv_timer_t * handle) {
    HandleScope scope;
    m_called = true;

    Handle<Value> argv[2] = {String::New("operation timeout"), Undefined()};
    m_callback->Call(Context::GetCurrent()->Global(), 2, argv);

    m_has_timer = false;
    uv_timer_stop(&m_timer);

    scope.Close(Undefined());
}

/************************************************************
 * Handles socket and timer events
 ************************************************************/
class Poller {
public:
    Poller(omcache_t *omc): m_omc(omc), m_dead(false) {}
    void Poll(Callback *);
    bool Die();
    ~Poller() {
        omcache_free(m_omc);
    }
private:
    // Handle socket events, dispatch to the callbacks
    static void HandleEvent(uv_poll_t *handle, int status, int event);
    void ProcessEvent(uv_poll_t * handle, int fd, int status, int event);
    void StopPolling(uv_poll_t *handle, int fd);

    // monitor callbacks, close unused polls, removed completed callbacks
    static void Idle(uv_idle_t *handle, int);
    void Cleanup(uv_idle_t *, uv_poll_t*, int fd);
    void StartIdle(uv_poll_t *poll_handle, int fd);
    void StopIdle(uv_idle_t *handle);

    struct PollData {
        int fd;
        Poller * poller;
    };

    struct IdleData {
        int fd;
        uv_poll_t * poll;
        Poller * poller;
    };

    typedef std::map<int, std::list<Callback *> > PollMap;
    PollMap m_polls;

    uv_idle_t * m_idle;
    omcache_t *m_omc;
    bool m_dead;
};

void Poller::Poll(Callback *cb) {
    uv_loop_t * loop = uv_default_loop();
    int nfds, polltimeout;
    pollfd * fds = omcache_poll_fds(m_omc, &nfds, &polltimeout);
    for (int i=0; i < nfds; ++i) {
        if (fds[i].events & (POLLIN | POLLOUT)) {
            int fd = fds[i].fd;
            PollMap::mapped_type &dest_list = m_polls[fd];
            bool start_polling = dest_list.empty();
            dest_list.push_back(cb);

            if (start_polling) {
                uv_poll_t * poll_handle = new uv_poll_t;
                PollData * poll_data = new PollData;

                poll_data->fd = fd;
                poll_data->poller = this;

                poll_handle->data = poll_data;
                uv_poll_init(loop, poll_handle, fd);
                uv_poll_start(poll_handle, UV_READABLE | UV_WRITABLE, HandleEvent);
                StartIdle(poll_handle, fd);
            }
        }
    }
}

bool Poller::Die() {
    m_dead = true;
    return m_polls.empty();
}

void Poller::ProcessEvent(uv_poll_t * handle, int fd, int status, int event) {
    if (!event) return;
    PollMap::iterator cb_it = m_polls.find(fd);
    if (cb_it == m_polls.end()) return;

    PollMap::mapped_type &callbacks = cb_it->second;
    PollMap::mapped_type::iterator it=callbacks.begin();
    while (it != callbacks.end()) {
        Callback * cb = *it;

        if (cb->Ping()) {
            callbacks.erase(it++);
            delete cb;
        } else {
            ++it;
        }
    }

    if (callbacks.empty()) {
        StopPolling(handle, fd);
    }

}

void Poller::HandleEvent(uv_poll_t *handle, int status, int event) {
{
    PollData * p = static_cast<PollData*>(handle->data);
    p->poller->ProcessEvent(handle, p->fd,  status, event);
 }
}

void Poller::StopPolling(uv_poll_t *handle, int fd) {
    uv_poll_stop(handle);
    delete (PollData*)handle->data;
    m_polls.erase(fd);
}

void Poller::StartIdle(uv_poll_t *poll_handle, int fd) {
    uv_idle_t * handle = new uv_idle_t;
    uv_idle_init(uv_default_loop(), handle);
    IdleData * data = new IdleData;
    data->fd = fd;
    data->poller = this;
    data->poll = poll_handle;
    handle->data = data;
    uv_idle_start(handle, Idle);
}

void Poller::Idle(uv_idle_t *handle, int) {
    IdleData * data = static_cast<IdleData*>(handle->data);
    data->poller->Cleanup(handle, data->poll, data->fd);
    if (data->poller->m_polls.empty() && data->poller->m_dead) {
        delete data->poller;
    }
}

void Poller::Cleanup(uv_idle_t *idle, uv_poll_t *poll, int fd) {
    if (!m_polls.count(fd)) {
        uv_idle_stop(idle);
        return;
    }

    PollMap::mapped_type &callbacks = m_polls[fd];
    PollMap::mapped_type::iterator it = callbacks.begin();
    while (it != callbacks.end()) {
        Callback * cb = *it;
        if (cb->Done()) {
            callbacks.erase(it++);
            delete cb;
        } else break;
    }

    if (callbacks.empty()) {
        uv_idle_stop(idle);
        uv_unref((uv_handle_t*)idle);
        StopPolling(poll, fd);
    }
}


/************************************************************
 * Javascript bindings
 ************************************************************/

class OMCache: public node::ObjectWrap {
public:
    OMCache(const std::string &servers, int timeout): m_timeout(timeout) {
        m_omc = omcache_init();
        m_poller = new Poller(m_omc);
        omcache_set_servers(m_omc, servers.c_str());
    }

    ~OMCache();

    static void Init(Handle<Object> exports);
    static OMCache * This(const Arguments &args);

    static Handle<Value> New(const Arguments &args);
    static Handle<Value> Get(const Arguments &args);
    static Handle<Value> Set(const Arguments &args);
    static Handle<Value> Increment(const Arguments &args);
    static Handle<Value> Decrement(const Arguments &args);

private:
    void Send(RequestTemplate &rt, const Local<Value> &callback);
    Handle<Value> Delta(const Arguments &args, int op);

    Poller * m_poller;
    omcache_t *m_omc;
    int m_timeout;
};

OMCache::~OMCache() {
    if (m_poller && m_poller->Die()) delete m_poller;
}

OMCache * OMCache::This(const Arguments &args) {
    return node::ObjectWrap::Unwrap<OMCache>(args.This());
}

void OMCache::Init(Handle<Object> exports) {
    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
    tpl->SetClassName(String::NewSymbol("OMCache"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    // Prototype
    tpl->PrototypeTemplate()->Set(String::NewSymbol("set"),
                                  FunctionTemplate::New(Set)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("get"),
                                  FunctionTemplate::New(Get)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("increment"),
                                  FunctionTemplate::New(Increment)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("decrement"),
                                  FunctionTemplate::New(Decrement)->GetFunction());

    exports->Set(String::NewSymbol("OMCache"), Persistent<Function>::New(tpl->GetFunction()));
}

static std::string ValueToString(const Local<Value>& val) {
    String::Utf8Value utf8(val->ToString());
    return std::string(*utf8);
}

Handle<Value> OMCache::New(const Arguments& args) {
    HandleScope scope;

    std::string servers;
    if (args[0]->IsArray()) {
        std::stringstream buf;
        Local<Array> srv_arrs = Local<Array>::Cast(args[0]);
        for (size_t i=0; i<srv_arrs->Length(); ++i) {
            if (i) buf << ',';
            buf << ValueToString(srv_arrs->Get(i));
        }
        servers = buf.str();
    } else {
        servers = ValueToString(args[0]);
    }

    int timeout = 0;
    if (args.Length() > 1 && args[1]->IsObject()) {
        Local<Object> options(args[1]->ToObject());
        Local<Integer> timeout_val = Local<Integer>::Cast(options->Get(String::New("timeout")));
        if (!timeout_val.IsEmpty()) {
            timeout = timeout_val->Value();
        }
    }

    OMCache * obj = new OMCache(servers, timeout);
    obj->Wrap(args.This());
    return args.This();
}

Handle<Value> OMCache::Set(const Arguments &args) {
    HandleScope scope;
    uint32_t extra[2] = {0};
    Local<Value> expiration = args[2];
    if (!expiration.IsEmpty()) {
        extra[1] = htonl(Local<Integer>::Cast(expiration)->Value());
    }

    RequestTemplate rt(CMD_SET, 0);
    rt.SetKey(args[0]).SetData(args[1]).SetExtra(extra, sizeof(extra));
    This(args)->Send(rt, args[3]);

    return scope.Close(Undefined());
}

Handle<Value> OMCache::Get(const Arguments &args) {
    HandleScope scope;
    RequestTemplate rt(CMD_GET, 0);
    rt.SetKey(args[0]);
    This(args)->Send(rt, args[1]);
    return scope.Close(Undefined());
}

Handle<Value> OMCache::Increment(const Arguments &args) {
    return This(args)->Delta(args, CMD_INC);
}

Handle<Value> OMCache::Decrement(const Arguments &args) {
    return This(args)->Delta(args, CMD_DEC);
}

Handle<Value> OMCache::Delta(const Arguments &args, int op) {
    HandleScope scope;
    RequestTemplate rt(op, 0);
    uint32_t extra[5] = {0};
    extra[1] = htonl(Local<Integer>::Cast(args[1])->Value());
    rt.SetKey(args[0]).SetExtra(extra, sizeof(extra));
    This(args)->Send(rt, args[2]);
    return scope.Close(Undefined());
}

void OMCache::Send(RequestTemplate &rt, const Local<Value> &callback) {
    Callback *cb = new Callback(m_omc);
    cb->SendCommand(rt, Persistent<Function>::New(Local<Function>::Cast(callback)), m_timeout);
    m_poller->Poll(cb);
}

NODE_MODULE(omcache, OMCache::Init)
