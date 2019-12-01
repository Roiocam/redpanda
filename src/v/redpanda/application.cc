#include "redpanda/application.h"

#include "platform/stop_signal.h"
#include "raft/service.h"
#include "storage/directories.h"
#include "syschecks/syschecks.h"
#include "utils/file_io.h"

#include <seastar/core/prometheus.hh>
#include <seastar/core/thread.hh>
#include <seastar/http/api_docs.hh>

#include <nlohmann/json.hpp>
#include <sys/utsname.h>

#include <chrono>

int application::run(int ac, char** av) {
    init_env();
    struct ::utsname buf;
    ::uname(&buf);
    _log.info(
      "kernel={}, nodename={}, machine={}",
      buf.release,
      buf.nodename,
      buf.machine);
    app_template app = setup_app_template();
    return app.run(ac, av, [this, &app] {
        auto& cfg = app.configuration();
        validate_arguments(cfg);
        return async([this, &cfg] {
            ::stop_signal app_signal;
            auto deferred = defer([this] {
                auto deferred = std::move(_deferred);
                // stop services in reverse order
                while (!deferred.empty()) {
                    deferred.pop_back();
                }
            });
            initialize();
            hydrate_config(cfg);
            check_environment();
            configure_admin_server();
            wire_up_services();
            start();
            app_signal.wait().get();
            _log.info("Stopping...");
        });
    });
}

void application::initialize() {
    _scheduling_groups.create_groups().get();
    _smp_groups.create_groups().get();
}

void application::validate_arguments(const po::variables_map& cfg) {
    if (!cfg.count("redpanda-cfg")) {
        throw std::invalid_argument("Missing redpanda-cfg flag");
    }
}

void application::init_env() { std::setvbuf(stdout, nullptr, _IOLBF, 1024); }

app_template application::setup_app_template() {
    app_template::config app_cfg;
    app_cfg.name = "Redpanda";
    using namespace std::literals::chrono_literals; // NOLINT
    app_cfg.default_task_quota = 500us;
    app_cfg.auto_handle_sigint_sigterm = false;
    auto app = app_template(app_cfg);
    app.add_options()(
      "redpanda-cfg",
      po::value<std::string>(),
      ".yaml file config for redpanda");
    return app;
}

void application::hydrate_config(const po::variables_map& cfg) {
    auto buf = read_fully(cfg["redpanda-cfg"].as<std::string>()).get0();
    // see https://github.com/jbeder/yaml-cpp/issues/765
    sstring workaround(sstring::initialized_later(), buf.size_bytes());
    auto in = iobuf::iterator_consumer(buf.cbegin(), buf.cend());
    in.consume_to(buf.size_bytes(), workaround.begin());
    YAML::Node config = YAML::Load(workaround);
    _log.info("Configuration:\n\n{}\n\n", config);
    smp::invoke_on_all([config] {
        config::shard_local_cfg().read_yaml(config);
    }).get0();
}

void application::check_environment() {
    auto& cfg = config::shard_local_cfg();
    syschecks::systemd_message("checking environment (CPU, Mem)");
    syschecks::cpu();
    syschecks::memory(config::shard_local_cfg().developer_mode());
    storage::directories::initialize(
      config::shard_local_cfg().data_directory().as_sstring())
      .get();
}

void application::configure_admin_server() {
    syschecks::systemd_message("constructing http server");
    construct_service(_admin, sstring("admin")).get();
    prometheus::config metrics_conf;
    metrics_conf.metric_help = "redpanda metrics";
    metrics_conf.prefix = "vectorized";
    prometheus::add_prometheus_routes(_admin, metrics_conf).get();
    auto& conf = config::shard_local_cfg();
    if (conf.enable_admin_api()) {
        syschecks::systemd_message(
          "enabling admin HTTP api: {}", config::shard_local_cfg().admin());
        auto rb = make_shared<api_registry_builder20>(
          conf.admin_api_doc_dir(), "/v1");
        _admin
          .invoke_on_all([rb, this](http_server& server) {
              rb->set_api_doc(server._routes);
              rb->register_api_file(server._routes, "header");
              rb->register_api_file(server._routes, "config");
              config_json::get_config.set(
                server._routes, [this](const_req req) {
                    nlohmann::json jconf;
                    config::shard_local_cfg().to_json(jconf);
                    return json::json_return_type(jconf.dump());
                });
          })
          .get();
    }

    with_scheduling_group(_scheduling_groups.admin_sg(), [this] {
        return _admin
          .invoke_on_all(
            &http_server::listen, config::shard_local_cfg().admin())
          .handle_exception([this](auto ep) {
              _log.error("Exception on http admin server: {}", ep);
              return make_exception_future<>(ep);
          });
    }).get();

    _log.info("Started HTTP admin service listening at {}", conf.admin());
}

// add additional services in here
void application::wire_up_services() {
    // cluster
    syschecks::systemd_message("Adding raft client cache");
    construct_service(_raft_client_cache).get();
    syschecks::systemd_message("Building shard-lookup tables");
    construct_service(shard_table).get();
    syschecks::systemd_message("Adding partition manager");
    construct_service(
      partition_manager,
      storage::log_append_config::fsync::yes,
      std::chrono::seconds(10), // disk timeout
      std::ref(shard_table),
      std::ref(_raft_client_cache))
      .get();
    _log.info("Partition manager started");

    // controller
    syschecks::systemd_message("Creating kafka metadata cache");
    construct_service(metadata_cache).get();

    syschecks::systemd_message("Creating cluster::controller");
    _controller = std::make_unique<cluster::controller>(
      partition_manager, shard_table, metadata_cache);
    _deferred.emplace_back([this] { _controller->stop().get(); });

    // group membership
    syschecks::systemd_message("Creating partition manager");
    construct_service(_group_manager, std::ref(partition_manager)).get();
    syschecks::systemd_message("Creating kafka group shard mapper");
    construct_service(_group_shard_mapper, std::ref(shard_table)).get();
    syschecks::systemd_message("Creating kafka group router");
    construct_service(
      group_router,
      _scheduling_groups.kafka_sg(),
      _smp_groups.kafka_smp_sg(),
      std::ref(_group_manager),
      std::ref(_group_shard_mapper))
      .get();

    // rpc
    rpc::server_configuration rpc_cfg;
    rpc_cfg.max_service_memory_per_core = memory_groups::rpc_total_memory();
    rpc_cfg.addrs.push_back(config::shard_local_cfg().rpc_server);
    syschecks::systemd_message(
      "Starting internal RPC {}", config::shard_local_cfg().rpc_server);
    construct_service(_rpc, rpc_cfg).get();

    // metrics and quota management
    syschecks::systemd_message("Adding kafka quota manager");
    construct_service(_quota_mgr).get();

    syschecks::systemd_message("Building kafka controller dispatcher");
    construct_service(
      cntrl_dispatcher,
      std::ref(*_controller),
      _smp_groups.kafka_smp_sg(),
      _scheduling_groups.kafka_sg())
      .get();
}

void application::start() {
    syschecks::systemd_message("Starting controller");
    _controller->start().get();

    syschecks::systemd_message("Starting RPC");
    _rpc
      .invoke_on_all([this](rpc::server& s) {
          s.register_service<
            raft::service<cluster::partition_manager, cluster::shard_table>>(
            _scheduling_groups.raft_sg(),
            _smp_groups.raft_smp_sg(),
            partition_manager,
            shard_table.local());
      })
      .get();
    auto& conf = config::shard_local_cfg();
    _rpc.invoke_on_all(&rpc::server::start).get();
    _log.info("Started RPC server listening at {}", conf.rpc_server());

    _quota_mgr.invoke_on_all(&kafka::quota_manager::start).get();

    // Kafka API
    syschecks::systemd_message("Building TLS credentials for kafka");
    auto kafka_creds = conf.kafka_api_tls().get_credentials_builder().get0();
    kafka::kafka_server_config server_config = {
      // FIXME: Add memory manager
      memory::stats().total_memory() / 10,
      _smp_groups.kafka_smp_sg(),
      kafka_creds};

    construct_service(
      _kafka_server,
      kafka::probe(),
      std::ref(metadata_cache),
      std::ref(cntrl_dispatcher),
      std::move(server_config),
      std::ref(_quota_mgr),
      std::ref(group_router),
      std::ref(shard_table),
      std::ref(partition_manager))
      .get();

    syschecks::systemd_message("Starting kafka api");
    _kafka_server
      .invoke_on_all([this](kafka::kafka_server& server) mutable {
          // FIXME: Configure keepalive.
          return server.listen(config::shard_local_cfg().kafka_api(), false);
      })
      .get();

    _log.info("Started Kafka API server listening at {}", conf.kafka_api());
    syschecks::systemd_message("redpanda ready!");
    syschecks::systemd_notify_ready();
}
