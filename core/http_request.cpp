#include "stdafx.h"
#include "http_request.h"
#include "../external/curl/include/curl.h"
#include "../external/openssl/openssl/crypto.h"
#include "tools/system.h"
#include "log/log.h"
#include "utils.h"
#include "async_task.h"
#include "core.h"
#include "proxy_settings.h"
#include "network_log.h"
#include "../corelib/enumerations.h"
#include "configuration/hosts_config.h"

using namespace core;

const int32_t default_http_connect_timeout = 15000; // 15 sec
const int32_t default_http_execute_timeout = 15000; // 15 sec

class http_handles : public core::ithread_callback
{
    std::map<boost::thread::id, CURL*> handles_;

    std::mutex sync_mutex_;

public:

    http_handles()
    {

    }

    virtual ~http_handles()
    {
        assert(handles_.empty());
    }

    bool initialize()
    {
        return true;
    }

    CURL* get_for_this_thread()
    {
        boost::thread::id thread_id = boost::this_thread::get_id();

        std::lock_guard<std::mutex> lock(sync_mutex_);

        auto iter_handle = handles_.find(thread_id);
        if (iter_handle != handles_.end())
            return iter_handle->second;

        CURL* curl_handle = curl_easy_init();

        assert(curl_handle);

        handles_[thread_id] = curl_handle;

        return curl_handle;
    }

    virtual void on_thread_shutdown()
    {
        boost::thread::id thread_id = boost::this_thread::get_id();

        std::lock_guard<std::mutex> lock(sync_mutex_);

        auto iter_handle = handles_.find(thread_id);
        if (iter_handle != handles_.end())
        {
            curl_easy_cleanup(iter_handle->second);

            handles_.erase(iter_handle);
        }
    }
};

http_handles* g_handles = 0;

static size_t write_header_function(void *contents, size_t size, size_t nmemb, void *userp);
static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp);
static int32_t progress_callback(void* ptr, double TotalToDownload, double NowDownloaded, double TotalToUpload, double NowUploaded);
static int32_t trace_function(CURL* _handle, curl_infotype _type, unsigned char* _data, size_t _size, void* _userp);

struct curl_context
{
    CURL* curl_;
    std::shared_ptr<tools::binary_stream> output_;
    std::shared_ptr<tools::binary_stream> header_;

    http_request_simple::stop_function stop_func_;

    http_request_simple::progress_function progress_func_;

    int32_t bytes_transferred_pct_;

    bool need_log_;
    std::shared_ptr<tools::binary_stream> log_data_;

    core::replace_log_function replace_log_function_;

    curl_slist* header_chunk_;
    struct curl_httppost* post;
    struct curl_httppost* last;

    bool keep_alive_;

    curl_context(http_request_simple::stop_function _stop_func, http_request_simple::progress_function _progress_func, bool _keep_alive)
        :
        curl_(_keep_alive ? g_handles->get_for_this_thread() : curl_easy_init()),
        output_(new core::tools::binary_stream()),
        header_(new core::tools::binary_stream()),
        log_data_(new core::tools::binary_stream()),
        stop_func_(_stop_func),
        progress_func_(_progress_func),
        header_chunk_(0),
        post(NULL),
        last(NULL),
        need_log_(true),
        keep_alive_(_keep_alive),
        replace_log_function_([](tools::binary_stream&){}),
        bytes_transferred_pct_(0)
    {
    }

    ~curl_context()
    {
        if (!keep_alive_)
            curl_easy_cleanup(curl_);

        if (header_chunk_)
            curl_slist_free_all(header_chunk_);
    }

    bool init(int32_t _connect_timeout, int32_t _timeout, core::proxy_settings _proxy_settings, const std::string &_user_agent)
    {
        assert(!_user_agent.empty());

        if (!curl_)
            return false;

        if (keep_alive_)
            curl_easy_reset(curl_);

        if (platform::is_windows() && !platform::is_windows_vista_or_late())
            curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
        else
            curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);

        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);

        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, (void *)this);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_memory_callback);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, (void *)this);
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, write_header_function);
        curl_easy_setopt(curl_, CURLOPT_PROGRESSDATA, (void *)this);
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, false);
        curl_easy_setopt(curl_, CURLOPT_PROGRESSFUNCTION, progress_callback);

        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, 5L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, 5L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "gzip");

        curl_easy_setopt(curl_, CURLOPT_USERAGENT, _user_agent.c_str());

        curl_easy_setopt(curl_, CURLOPT_DEBUGDATA, (void*) this);
        curl_easy_setopt(curl_, CURLOPT_DEBUGFUNCTION, trace_function);
        curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1L);

        if (_timeout > 0)
            curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, _timeout);

        if (_connect_timeout > 0)
            curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, _connect_timeout);

        if (_proxy_settings.use_proxy_)
        {
            curl_easy_setopt(curl_, CURLOPT_PROXY, tools::from_utf16(_proxy_settings.proxy_server_).c_str());

            if (_proxy_settings.proxy_port_ != proxy_settings::default_proxy_port)
            {
                curl_easy_setopt(curl_, CURLOPT_PROXYPORT, _proxy_settings.proxy_port_);
            }
            curl_easy_setopt(curl_, CURLOPT_PROXYTYPE, _proxy_settings.proxy_type_);

            if (_proxy_settings.need_auth_)
            {
                curl_easy_setopt(curl_, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
                curl_easy_setopt(curl_, CURLOPT_PROXYUSERPWD, tools::from_utf16(_proxy_settings.login_ + L":" + _proxy_settings.password_).c_str());
            }
        }

        return true;
    }

    void set_replace_log_function(replace_log_function _func)
    {
        replace_log_function_ = _func;
    }

    bool is_need_log()
    {
        return need_log_;
    }

    void set_need_log(bool _need)
    {
        need_log_ = _need;
    }

    void write_log_data(const char* _data, uint32_t _size)
    {
        log_data_->write(_data, _size);
    }

    void write_log_string(const std::string& _log_string)
    {
        log_data_->write<std::string>(_log_string);
    }

    void set_range(int64_t _from, int64_t _to)
    {
        assert(_from >= 0);
        assert(_to > 0);
        assert(_from < _to);

        std::stringstream ss_range;
        ss_range << _from << "-" << _to;

        curl_easy_setopt(curl_, CURLOPT_RANGE, ss_range.str().c_str());
    }

    void set_url(const char* sz_url)
    {
        curl_easy_setopt(curl_, CURLOPT_URL, sz_url);

        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 10);

        if (!keep_alive_)
        {
            curl_easy_setopt(curl_, CURLOPT_COOKIEFILE, "");
        }
    }

    void set_post()
    {
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    }

    void set_http_post()
    {
        curl_easy_setopt(curl_, CURLOPT_HTTPPOST, post);
    }

    void set_post_fields(const char* sz_fields, int32_t _size)
    {
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, _size);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, sz_fields);
    }

    long get_response_code()
    {
        long response_code = 0;

        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &response_code);

        return response_code;
    }

    void set_modified_time(time_t _last_modified_time)
    {
        curl_easy_setopt(curl_, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
        curl_easy_setopt(curl_, CURLOPT_TIMEVALUE, _last_modified_time);
    }

    std::shared_ptr<tools::binary_stream> get_response()
    {
        return output_;
    }

    std::shared_ptr<tools::binary_stream> get_header()
    {
        return header_;
    }

    void set_form_field(const char* _field_name, const char* _value)
    {
        curl_formadd(&post, &last, CURLFORM_COPYNAME, _field_name,
            CURLFORM_COPYCONTENTS, _value, CURLFORM_END);
    }

    void set_form_file(const char* _field_name, const char* _file_name)
    {
        curl_formadd(&post, &last, CURLFORM_COPYNAME, _field_name, CURLFORM_FILE, _file_name,
            CURLFORM_END);
    }

    void set_form_filedata(const char* _field_name, const char* _file_name, tools::binary_stream& _data)
    {
        long size = _data.available();//it should be long
        const auto data = _data.read_available();
        _data.reset_out();
        curl_formadd(&post, &last,
                     CURLFORM_COPYNAME, _field_name,
                     CURLFORM_BUFFER, _file_name,
                     CURLFORM_BUFFERPTR, data,
                     CURLFORM_BUFFERLENGTH, size,
                     CURLFORM_CONTENTTYPE, "application/octet-stream",
                     CURLFORM_END);
    }

    bool execute_request()
    {
        CURLcode res = curl_easy_perform(curl_);

        std::stringstream error;
        error << "curl_easy_perform result is ";
        error << res;
        error << std::endl;

        write_log_string(error.str());

        replace_log_function_(*log_data_);

        g_core->get_network_log().write_data(*log_data_);

        return (res == CURLE_OK);
    }

    void set_custom_header_params(const std::list<std::string>& _params)
    {
        for (auto parameter : _params)
            header_chunk_ = curl_slist_append(header_chunk_, parameter.c_str());

        if (header_chunk_)
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_chunk_);
    }
};

static size_t write_header_function(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    curl_context *ctx = (curl_context *)userp;
    ctx->header_->reserve((uint32_t)realsize);
    ctx->header_->write((char *)contents, (uint32_t)realsize);

    return realsize;
}

static size_t write_memory_callback(void* _contents, size_t _size, size_t _nmemb, void* _userp)
{
    size_t realsize = _size * _nmemb;
    curl_context* ctx = (curl_context*) _userp;
    ctx->output_->write((char*) _contents, (uint32_t) realsize);

    if (ctx->is_need_log())
    {
        ctx->write_log_data((const char*) _contents, (uint32_t) realsize);
    }

    return realsize;
}

static int32_t progress_callback(void* ptr, double TotalToDownload, double NowDownloaded, double TotalToUpload, double /*NowUploaded*/)
{
    auto cntx = (curl_context*)ptr;

    assert(cntx->bytes_transferred_pct_ >= 0);
    assert(cntx->bytes_transferred_pct_ <= 100);

    if (cntx->stop_func_ && cntx->stop_func_())
    {
        return -1;
    }

    const auto file_too_small = (TotalToDownload <= 1);
    const auto skip_progress = (file_too_small || !cntx->progress_func_);
    if (skip_progress)
    {
        return 0;
    }

    const auto bytes_transferred_pct = (int32_t)((NowDownloaded * 100) / TotalToDownload);
    assert(bytes_transferred_pct >= 0);
    assert(bytes_transferred_pct <= 100);

    const auto percentage_updated = (bytes_transferred_pct != cntx->bytes_transferred_pct_);
    if (!percentage_updated)
    {
        return 0;
    }

    cntx->bytes_transferred_pct_ = bytes_transferred_pct;

    cntx->progress_func_((int64_t)TotalToDownload, (int64_t)NowDownloaded, bytes_transferred_pct);

    return 0;
}

std::vector<std::string> get_filter_keywords()
{
    std::vector<std::string> keywords;
    keywords.push_back("schannel:");
    keywords.push_back("STATE:");

    return keywords;
}

const std::string filter1 = "schannel:";
const std::string filter2 = "STATE:";

static int32_t trace_function(CURL* _handle, curl_infotype _type, unsigned char* _data, size_t _size, void* _userp)
{
    curl_context* ctx = (curl_context*) _userp;
    if (!ctx->is_need_log())
        return 0;

    std::stringstream ss;

    const char *text = "";

    (void)_userp;
    (void)_handle; /* prevent compiler warning */

    switch (_type)
    {
    case CURLINFO_TEXT:
        {
            if (filter1.length() < _size && memcmp(_data, filter1.c_str(), filter1.length()) == 0)
                return 0;

            if (filter2.length() < _size && memcmp(_data, filter2.c_str(), filter2.length()) == 0)
                return 0;
        }
        break;
    case CURLINFO_HEADER_OUT:
    //    text = "=> Send header";
        break;
    case CURLINFO_DATA_OUT:
    case CURLINFO_SSL_DATA_OUT:
    //    text = "=> Send data";
        break;
    case CURLINFO_HEADER_IN:
    //    text = "<= Recv header";
        break;
    case CURLINFO_DATA_IN:
    case CURLINFO_SSL_DATA_IN:
    //    text = "<= Recv data";
        return 0;
    default:
        break;
    }

    tools::binary_stream bs;

    if (*text != '\0')
    {
        ctx->write_log_string(text);
        ctx->write_log_string("\n");
    }

    ctx->write_log_data((const char*) _data, (uint32_t) _size);

    return 0;
}

http_request_simple::http_request_simple(proxy_settings _proxy_settings, const std::string &_user_agent, stop_function _stop_func, progress_function _progress_func)
    : stop_func_(_stop_func),
    output_(new tools::binary_stream()),
    header_(new tools::binary_stream()),
    is_time_condition_(false),
    last_modified_time_(0),
    post_data_(0),
    post_data_size_(0),
    copy_post_data_(false),
    timeout_(default_http_execute_timeout),
    connect_timeout_(default_http_connect_timeout),
    range_from_(-1),
    range_to_(-1),
    is_post_form_(false),
    need_log_(true),
    keep_alive_(false),
    proxy_settings_(_proxy_settings),
    user_agent_(_user_agent),
    replace_log_function_([](tools::binary_stream&){}),
    progress_func_(_progress_func)
{
    assert(!user_agent_.empty());
}

http_request_simple::~http_request_simple()
{
    clear_post_data();
}

void http_request_simple::set_need_log(bool _need)
{
    need_log_ = _need;
}

void http_request_simple::push_post_parameter(const std::wstring& name, const std::wstring& value)
{
    assert(!name.empty());

    post_parameters_[tools::from_utf16(name)] = tools::from_utf16(value);
}

void http_request_simple::push_post_parameter(const std::string& name, const std::string& value)
{
    post_parameters_[name] = value;
}

void http_request_simple::push_post_form_parameter(const std::wstring& name, const std::wstring& value)
{
    post_form_parameters_[tools::from_utf16(name)] = tools::from_utf16(value);
}

void http_request_simple::push_post_form_parameter(const std::string& name, const std::string& value)
{
    assert(!name.empty());
    post_form_parameters_[name] = value;
}

void http_request_simple::push_post_form_file(const std::wstring& name, const std::wstring& file_name)
{
    push_post_form_file(tools::from_utf16(name), tools::from_utf16(file_name));
}

void http_request_simple::push_post_form_file(const std::string& name, const std::string& file_name)
{
    assert(!name.empty());
    assert(!file_name.empty());
    post_form_files_.insert(std::make_pair(name, file_name));
}

void http_request_simple::push_post_form_filedata(const std::wstring& name, const std::wstring& file_name)
{
    assert(!name.empty());
    assert(!file_name.empty());
    file_binary_stream file_data;
    file_data.file_name_ = file_name.substr(file_name.find_last_of(L"\\/") + 1);
    file_data.file_stream_.load_from_file(file_name);
    if (file_data.file_stream_.available())
        post_form_filedatas_.insert(std::make_pair(tools::from_utf16(name), file_data));
}

void http_request_simple::set_url(const std::wstring& url)
{
    set_url(tools::from_utf16(url));
}

void http_request_simple::set_url(const std::string& url)
{
    url_ = url;
}

void http_request_simple::set_modified_time_condition(time_t _modified_time)
{
    is_time_condition_ = true;
    last_modified_time_ = _modified_time;
}

void http_request_simple::set_connect_timeout(int32_t _timeout_ms)
{
    connect_timeout_ = _timeout_ms;
}

void http_request_simple::set_timeout(int32_t _timeout_ms)
{
    timeout_ = _timeout_ms;
}

std::string http_request_simple::get_post_param() const
{
    std::string result = "";
    if (!post_parameters_.empty())
    {
        std::stringstream ss_post_params;

        for (auto iter = post_parameters_.begin(); iter != post_parameters_.end(); ++iter)
        {
            if (iter != post_parameters_.begin())
                ss_post_params << '&';

            ss_post_params << iter->first;

            if (!iter->second.empty())
                ss_post_params << '=' << iter->second;
        }

        result = ss_post_params.str();
    }
    return result;
}

void http_request_simple::set_post_params(curl_context* _ctx)
{
    auto post_params = get_post_param();

    if (!post_params.empty())
        set_post_data(post_params.c_str(), (int32_t)post_params.length(), true);

    for (auto iter = post_form_parameters_.begin(); iter != post_form_parameters_.end(); ++iter)
    {
        if (!iter->second.empty())
            _ctx->set_form_field(iter->first.c_str(), iter->second.c_str());
    }

    for (auto iter = post_form_files_.begin(); iter != post_form_files_.end(); ++iter)
    {
        _ctx->set_form_file(iter->first.c_str(), iter->second.c_str());
    }

    for (auto iter = post_form_filedatas_.begin(); iter != post_form_filedatas_.end(); ++iter)
    {
        _ctx->set_form_filedata(iter->first.c_str(), tools::from_utf16(iter->second.file_name_).c_str(), iter->second.file_stream_);
    }

    if (post_data_ && post_data_size_)
        _ctx->set_post_fields(post_data_, post_data_size_);

    if (is_post_form_)
        _ctx->set_http_post();
    else
        _ctx->set_post();
}

bool http_request_simple::send_request(bool _post, bool switch_proxy)
{
    curl_context ctx(stop_func_, progress_func_, keep_alive_);

    auto is_user_proxy = proxy_settings_.proxy_type_ != (int32_t)core::proxy_types::auto_proxy;

    auto current_proxy = switch_proxy ? g_core->get_registry_proxy_settings() : g_core->get_auto_proxy_settings();
    if (is_user_proxy)
        current_proxy = proxy_settings_;

    if (!ctx.init(connect_timeout_, timeout_, current_proxy, user_agent_))
    {
        assert(false);
        return false;
    }

    if (_post)
        set_post_params(&ctx);

    if (is_time_condition_)
        ctx.set_modified_time(last_modified_time_);

    if (range_from_ >= 0 && range_to_ > 0)
        ctx.set_range(range_from_, range_to_);

    ctx.set_need_log(need_log_);

    ctx.set_custom_header_params(custom_headers_);
    ctx.set_url(url_.c_str());

    ctx.set_replace_log_function(replace_log_function_);

    if (is_user_proxy)
    {
        if (!ctx.execute_request())
            return false;
    }
    else
    {
        static std::atomic<bool> first(true);
        if (!ctx.execute_request())
        {
            if (first)
            {
                if (switch_proxy)
                    return false;

                return send_request(_post, true);
            }
            return false;
        }

        first = false;
        if (switch_proxy)
            g_core->switch_proxy_settings();
    }



    response_code_ = ctx.get_response_code();
    output_ = ctx.get_response();
    header_ = ctx.get_header();

    return true;
}

bool http_request_simple::post()
{
    return send_request(true);
}

bool http_request_simple::get()
{
    return send_request(false);
}

void http_request_simple::set_range(int64_t _from, int64_t _to)
{
    range_from_ = _from;
    range_to_ = _to;
}

std::shared_ptr<tools::binary_stream> http_request_simple::get_response()
{
    return output_;
}

std::shared_ptr<tools::binary_stream> http_request_simple::get_header()
{
    return header_;
}

long http_request_simple::get_response_code()
{
    return response_code_;
}

void http_request_simple::get_post_parameters(std::map<std::string, std::string>& _params)
{
    _params = post_parameters_;
}

void http_request_simple::set_custom_header_param(const std::string& _value)
{
    custom_headers_.push_back(_value);
}

void http_request_simple::clear_post_data()
{
    if (copy_post_data_ && post_data_)
        free(post_data_);

    copy_post_data_ = false;
    post_data_ = 0;
    post_data_size_ = 0;
}

void http_request_simple::set_post_data(const char* _data, int32_t _size, bool _copy_post_data)
{
    assert(_data);
    assert(_size);


    clear_post_data();

    copy_post_data_ = _copy_post_data;
    post_data_size_ = _size;

    if (_copy_post_data)
    {
        post_data_ = (char*) malloc(_size);
        memcpy(post_data_, _data, _size);
    }
    else
    {
        post_data_ = (char*) _data;
    }
}


std::vector<std::mutex*> http_request_simple::ssl_sync_objects;

static unsigned long id_function(void)
{
    return tools::system::get_current_thread_id();
}


static void locking_function( int32_t mode, int32_t n, const char *file, int32_t line )
{
    if ( mode & CRYPTO_LOCK )
        http_request_simple::ssl_sync_objects[n]->lock();
    else
        http_request_simple::ssl_sync_objects[n]->unlock();
}


void core::http_request_simple::init_global()
{
    auto lock_count = CRYPTO_num_locks();
    http_request_simple::ssl_sync_objects.resize(lock_count);
    for (auto i = 0;  i < lock_count;  i++)
        http_request_simple::ssl_sync_objects[i] = new std::mutex();

    CRYPTO_set_id_callback(id_function);
    CRYPTO_set_locking_callback(locking_function);

#ifdef _WIN32
    curl_global_init(CURL_GLOBAL_ALL);
#else
    curl_global_init(CURL_GLOBAL_SSL);
#endif
}

void core::http_request_simple::shutdown_global()
{
    if (!http_request_simple::ssl_sync_objects.size())
        return;

    CRYPTO_set_id_callback(NULL);
    CRYPTO_set_locking_callback(NULL);

    for (auto i = 0;  i < CRYPTO_num_locks(  );  i++)
        delete(http_request_simple::ssl_sync_objects[i]);

    http_request_simple::ssl_sync_objects.clear();
}

void core::http_request_simple::set_post_form(bool _is_post_form)
{
    is_post_form_ = _is_post_form;
}

void core::http_request_simple::set_keep_alive()
{
    if (keep_alive_)
        return;

    keep_alive_ = true;

    custom_headers_.push_back("Connection: keep-alive");
}

void core::http_request_simple::set_etag(const char *etag)
{
    if (etag && strlen(etag))
    {
        std::stringstream ss;
        ss << "If-None-Match: \"" << etag << "\"";
        custom_headers_.push_back(ss.str().c_str());
    }
}

void core::http_request_simple::set_replace_log_function(replace_log_function _func)
{
    replace_log_function_ = _func;
}


ithread_callback* core::http_request_simple::create_http_handlers()
{
    assert(!g_handles);
    g_handles = new http_handles();
    return g_handles;
}

std::string core::http_request_simple::get_post_url()
{
    return url_ + "?" + get_post_param();
}

proxy_settings core::http_request_simple::get_user_proxy() const
{
    return proxy_settings_;
}

void core::http_request_simple::replace_host(const hosts_map& _hosts)
{
    std::string::const_iterator it_prot = url_.begin();

    size_t pos_start = 0;

    auto pos_prot = url_.find("://");
    if (pos_prot != std::string::npos)
    {
        pos_start = pos_prot + 3;
    }

    if (pos_prot >= url_.length())
        return;

    std::stringstream ss_host;

    size_t pos_end = pos_start;

    while (url_[pos_end] != '/' && url_[pos_end] != '?' && url_[pos_end] != ':' && pos_end < url_.length())
    {
        ss_host << url_[pos_end];
        ++pos_end;
    }

    const auto host = ss_host.str();

    if (host.empty())
        return;

    url_.replace(pos_start, pos_end - pos_start, _hosts.get_host_alt(host));
}
