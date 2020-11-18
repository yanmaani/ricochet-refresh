#include "context.hpp"
#include "error.hpp"
#include "globals.hpp"
#include "tor.hpp"

using tego::g_globals;

#include "tor/TorControl.h"
#include "tor/TorManager.h"
#include "tor/TorProcess.h"

//
// Tego Context
//

tego_context::tego_context()
: callback_registry_(this)
, callback_queue_(this)
{
    this->torManager = Tor::TorManager::instance();
    this->torControl = torManager->control();
}

void tego_context::start_tor(const tego_tor_launch_config_t* config)
{
    TEGO_THROW_IF_NULL(config);

    this->torManager->setDataDirectory(config->dataDirectory.data());
    this->torManager->start();
}

bool tego_context::get_tor_daemon_configured() const
{
    TEGO_THROW_IF_NULL(this->torManager);

    return !this->torManager->configurationNeeded();
}

size_t tego_context::get_tor_logs_size() const
{
    size_t retval = 0;
    for(const auto& msg : this->get_tor_logs())
    {
        retval += msg.size() + 1;
    }
    return retval;
}

const std::vector<std::string>& tego_context::get_tor_logs() const
{
    TEGO_THROW_IF_NULL(this->torManager);

    auto logMessages = this->torManager->logMessages();
    TEGO_THROW_IF_FALSE(logMessages.size() >= 0);

    // see if we need to update our local copy
    if(static_cast<size_t>(logMessages.size()) != this->torLogs.size())
    {
        for(size_t i = this->torLogs.size(); i < static_cast<size_t>(logMessages.size()); i++)
        {
            this->torLogs.push_back(std::move(logMessages[i].toStdString()));
        }
    }

    return this->torLogs;
}

const char* tego_context::get_tor_version_string() const
{
    if (torVersion.empty())
    {
        TEGO_THROW_IF_NULL(this->torControl);

        QString torVersion = this->torControl->torVersion();
        this->torVersion = torVersion.toStdString();
    }

    return this->torVersion.c_str();
}

tego_tor_control_status_t tego_context::get_tor_control_status() const
{
    TEGO_THROW_IF_NULL(this->torControl);
    return static_cast<tego_tor_control_status_t>(this->torControl->status());
}

tego_tor_process_status_t tego_context::get_tor_process_status() const
{
    TEGO_THROW_IF_NULL(this->torManager);

    auto torProcess = this->torManager->process();
    if (torProcess == nullptr)
    {
        return tego_tor_process_status_external;
    }

    switch(torProcess->state())
    {
        case Tor::TorProcess::Failed:
            return tego_tor_process_status_failed;
        case Tor::TorProcess::NotStarted:
            return tego_tor_process_status_not_started;
        case Tor::TorProcess::Starting:
            return tego_tor_process_status_starting;
        case Tor::TorProcess::Connecting:
            // fall through, Connecting is really control status and not used by frontend explicitly
        case Tor::TorProcess::Ready:
            return tego_tor_process_status_running;
    }

    return tego_tor_process_status_unknown;
}

tego_tor_network_status_t tego_context::get_tor_network_status() const
{
    TEGO_THROW_IF_NULL(this->torControl);
    switch(this->torControl->torStatus())
    {
        case Tor::TorControl::TorOffline:
            return tego_tor_network_status_offline;
        case Tor::TorControl::TorReady:
            return tego_tor_network_status_ready;
        default:
            return tego_tor_network_status_unknown;
    }
}

tego_tor_bootstrap_tag_t tego_context::get_tor_bootstrap_tag() const
{
    TEGO_THROW_IF_NULL(this->torControl);
    auto bootstrapStatus = this->torControl->bootstrapStatus();
    auto bootstrapTag = bootstrapStatus["tag"].toString();

    // see https://gitweb.torproject.org/torspec.git/tree/control-spec.txt#n3867
    // TODO: optimize this function if you're bored, could do binary search rather than linear search
    constexpr static const char* tagList[] =
    {
        "starting",
        "conn_pt",
        "conn_done_pt",
        "conn_proxy",
        "conn_done_proxy",
        "conn",
        "conn_done",
        "handshake",
        "handshake_done",
        "onehop_create",
        "requesting_status",
        "loading_status",
        "loading_keys",
        "requesting_descriptors",
        "loading_descriptors",
        "enough_dirinfo",
        "ap_conn_pt_summary",
        "ap_conn_done_pt",
        "ap_conn_proxy",
        "ap_conn_done_proxy",
        "ap_conn",
        "ap_conn_done",
        "ap_handshake",
        "ap_handshake_done",
        "circuit_create",
        "done",
    };
    static_assert(tego::countof(tagList) == tego_tor_bootstrap_tag_count);

    for(size_t i = 0; i < tego_tor_bootstrap_tag_count; i++)
    {
        if (tagList[i] == bootstrapTag)
        {
            return static_cast<tego_tor_bootstrap_tag_t>(i);
        }
    }

    TEGO_THROW_MSG("unrecognized bootstrap tag : \"{}\"", bootstrapTag);
}

int32_t tego_context::get_tor_bootstrap_progress() const
{
    TEGO_THROW_IF_NULL(this->torControl);
    auto bootstrapStatus = this->torControl->bootstrapStatus();
    auto bootstrapProgress = bootstrapStatus["progress"].toInt();
    return static_cast<int32_t>(bootstrapProgress);
}

void tego_context::update_tor_daemon_config(const tego_tor_daemon_config_t* daemonConfig)
{
    TEGO_THROW_IF_NULL(this->torControl);

    const auto& config = *daemonConfig;

    QVariantMap vm;

    // init the tor settings we can modify here
    constexpr static auto configKeys =
    {
        "DisableNetwork",
        "Socks4Proxy",
        "Socks5Proxy",
        "Socks5ProxyUsername",
        "Socks5ProxyPassword",
        "HTTPSProxy",
        "HTTPSProxyAuthenticator",
        "ReachableAddresses",
        "Bridge",
        "UseBridges",
    };
    for(const auto& currentKey : configKeys)
    {
        vm[currentKey] = "";
    }

    // set disable network flag
    if (config.disableNetwork.has_value()) {
        vm["DisableNetwork"] = (config.disableNetwork.value() ? "1" : "0");
    }

    // set proxy info
    switch(config.proxy.type)
    {
        case tego_proxy_type_none: break;
        case tego_proxy_type_socks4:
        {
            vm["Socks4Proxy"] = QString::fromStdString(fmt::format("{}:{}", config.proxy.address, config.proxy.port));
        }
        break;
        case tego_proxy_type_socks5:
        {
            vm["Socks5Proxy"] = QString::fromStdString(fmt::format("{}:{}", config.proxy.address, config.proxy.port));
            if (!config.proxy.username.empty())
            {
                vm["Socks5ProxyUsername"] = QString::fromStdString(config.proxy.username);
            }
            if (!config.proxy.password.empty())
            {
                vm["Socks5ProxyPassword"] = QString::fromStdString(config.proxy.password);
            }
        }
        break;
        case tego_proxy_type_https:
        {
            vm["HTTPSProxy"] = QString::fromStdString(fmt::format("{}:{}", config.proxy.address, config.proxy.port));
            if (!config.proxy.username.empty() || !config.proxy.password.empty())
            {
                vm["HTTPSProxyAuthenticator"] = QString::fromStdString(fmt::format("{}:{}", config.proxy.username, config.proxy.password));
            }
        }
        break;
    }

    // set firewall ports
    if (config.allowedPorts.size() > 0)
    {
        std::stringstream ss;

        auto it = config.allowedPorts.begin();

        ss << fmt::format("*:{}", *it);
        for(++it; it < config.allowedPorts.end(); ++it)
        {
            ss << fmt::format(", *:{}", *it);
        }

        vm["ReachableAddresses"] = QString::fromStdString(ss.str());
    }

    // set bridges
    if (config.bridges.size() > 0)
    {
        QVariantList bridges;
        for(const auto& currentBridge : config.bridges)
        {
            bridges.append(QString::fromStdString(currentBridge));
        }
        vm["Bridge"] = bridges;
        vm["UseBridges"] = "1";
    }

    this->torControl->setConfiguration(vm);
}

void tego_context::save_tor_daemon_config()
{
    TEGO_THROW_IF_NULL(this->torControl);
    logger::trace();
    this->torControl->saveConfiguration();
}

//
// Exports
//

extern "C"
{
    // Bootstrap Tag

    const char* tego_tor_bootstrap_tag_to_summary(
        tego_tor_bootstrap_tag_t tag,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> const char*
        {
            TEGO_THROW_IF_FALSE(tag > tego_tor_bootstrap_tag_invalid &&
                                tag < tego_tor_bootstrap_tag_count);

            // strings from: https://gitweb.torproject.org/torspec.git/tree/control-spec.txt#n3867
            constexpr static const char* summaryList[] =
            {
                "Starting",
                "Connecting to pluggable transport",
                "Connected to pluggable transport",
                "Connecting to proxy",
                "Connected to proxy",
                "Connecting to a relay",
                "Connected to a relay",
                "Handshaking with a relay",
                "Handshake with a relay done",
                "Establishing an encrypted directory connection",
                "Asking for networkstatus consensus",
                "Loading networkstatus consensus",
                "Loading authority key certs",
                "Asking for relay descriptors",
                "Loading relay descriptors",
                "Loaded enough directory info to build circuits",
                "Connecting to pluggable transport to build circuits",
                "Connected to pluggable transport to build circuits",
                "Connecting to proxy to build circuits",
                "Connected to proxy to build circuits",
                "Connecting to a relay to build circuits",
                "Connected to a relay to build circuits",
                "Finishing handshake with a relay to build circuits",
                "Handshake finished with a relay to build circuits",
                "Establishing a Tor circuit",
                "Done",
            };
            static_assert(tego::countof(summaryList) == tego_tor_bootstrap_tag_count);

            return summaryList[static_cast<size_t>(tag)];
        }, error, nullptr);
    }

    // Tego Context

    void tego_context_start_tor(
        tego_context_t* context,
        const tego_tor_launch_config_t* launchConfig,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> void
        {
            TEGO_THROW_IF_NULL(context);

            context->start_tor(launchConfig);
        }, error);
    }

    void tego_context_get_tor_daemon_configured(
        const tego_context_t* context,
        tego_bool_t* out_configured,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> void
        {
            TEGO_THROW_IF_NULL(context);
            TEGO_THROW_IF_NULL(out_configured);

            *out_configured = TEGO_FALSE;

            if(context->get_tor_daemon_configured())
            {
                *out_configured = TEGO_TRUE;
            }
        }, error);
    };

    size_t tego_context_get_tor_logs_size(
        const tego_context_t* context,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> size_t
        {
            TEGO_THROW_IF_NULL(context);

            return context->get_tor_logs_size();
        }, error, 0);
    }


    size_t tego_context_get_tor_logs(
        const tego_context_t* context,
        char* out_logBuffer,
        size_t logBufferSize,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> size_t
        {
            TEGO_THROW_IF_NULL(context);
            TEGO_THROW_IF_FALSE(out_logBuffer);

            // get our tor logs
            const auto& logs = context->get_tor_logs();
            size_t logsSize = context->get_tor_logs_size();

            // create temporary buffer to copy each line into
            std::vector<char> logBuffer;
            logBuffer.reserve(logsSize);

            // copy each log and separate by new lines '\n'
            for(const auto& line : logs)
            {
                std::copy(line.begin(), line.end(), std::back_inserter(logBuffer));
                logBuffer.push_back('\n');
            }
            // append null terminator
            logBuffer.push_back(0);

            // finally copy at most logBufferSize bytes from logBuffer
            size_t copyCount = std::min(logBufferSize, logBuffer.size());
            std::copy(logBuffer.begin(), logBuffer.begin() + copyCount, out_logBuffer);
            // always write null terminator at the end
            out_logBuffer[copyCount - 1] = 0;

            return copyCount;
        }, error, 0);
    }

    const char* tego_context_get_tor_version_string(
        const tego_context_t* context,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> const char*
        {
            TEGO_THROW_IF_NULL(context);

            return context->get_tor_version_string();
        }, error, nullptr);
    }

    void tego_context_get_tor_control_status(
        const tego_context_t* context,
        tego_tor_control_status_t* out_status,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> void
        {
            TEGO_THROW_IF_NULL(context);
            TEGO_THROW_IF_NULL(out_status);

            auto status = context->get_tor_control_status();
            *out_status = status;
        }, error);
    }

    void tego_context_get_tor_process_status(
        const tego_context_t* context,
        tego_tor_process_status_t* out_status,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> void
        {
            TEGO_THROW_IF_NULL(context);
            TEGO_THROW_IF_NULL(out_status);

            const auto status = context->get_tor_process_status();
            *out_status = status;
        }, error);
    }

    void tego_context_get_tor_network_status(
        const tego_context_t* context,
        tego_tor_network_status_t* out_status,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> void
        {
            TEGO_THROW_IF_NULL(context);
            TEGO_THROW_IF_NULL(out_status);

            auto status = context->get_tor_network_status();
            *out_status = status;
        }, error);
    }

    void tego_context_get_tor_bootstrap_status(
        const tego_context* context,
        int32_t* out_progress,
        tego_tor_bootstrap_tag_t* out_tag,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> void
        {
            TEGO_THROW_IF_NULL(context);
            TEGO_THROW_IF_NULL(out_progress);
            TEGO_THROW_IF_NULL(out_tag);

            auto progress = context->get_tor_bootstrap_progress();
            auto tag = context->get_tor_bootstrap_tag();

            *out_progress = progress;
            *out_tag = tag;
        }, error);
    }

    void tego_context_update_tor_daemon_config(
        tego_context_t* context,
        const tego_tor_daemon_config_t* torConfig,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> void
        {
            TEGO_THROW_IF_NULL(context);

            context->update_tor_daemon_config(torConfig);
        }, error);
    }

    void tego_context_save_tor_daemon_config(
        tego_context_t* context,
        tego_error_t** error)
    {
        return tego::translateExceptions([=]() -> void
        {
            TEGO_THROW_IF_NULL(context);

            context->save_tor_daemon_config();
        }, error);
    }
}