#pragma once

#include "cluster/metadata_cache.h"
#include "cluster/metadata_dissemination_types.h"
#include "cluster/partition_manager.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "raft/group_manager.h"
#include "raft/types.h"
#include "rpc/connection_cache.h"
#include "utils/mutex.h"
#include "utils/retry.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/sharded.hh>

#include <absl/container/flat_hash_map.h>

namespace cluster {

/// Implementation of metadata dissemination service.
/// This service handles leadership updates on remote nodes that doesn't have
/// instances of raft group that current node have. Instace of raft group
/// triggers leadership notification and by that mean updates leadership in
/// metadata cache.
/// The service caches all leadership updates and sends them as
/// batch per node, every configurable period of time. This service is also
/// responsible for querying one of the cluster nodes for current leadership
/// metadata when node has started.
///
/// Used acronymes:
/// RG<num> - raft group with <num> id
///
/// Exemplary dissemination scenario:
///
///
/// - RG1 has replication factor of 3 is handled at nodes [1,2,3]
/// - Cluster contain five nodes [1,2,3,4,5]
/// - New leader for RG1 was elected, node 2 is new leader
/// - Information about leadership is available on each node that have RG1
///   instance
/// - Nodes without RG1 instance (non overlapping nodes) [4,5]
/// - Dissemination service will distribute metadata information to nodes 4 & 5
///
///                    Dissemination requests <RG1 leader = 2>
///                    +--------------------------------------+
///                    |                                      |
///                    +-------------------------+            |
///                    |                         v            v
/// +1--------+   +2--------+  +3--------+  +4---+----+  +5---+----+
/// | +-----+ |   | +-----+ |  | +-----+ |  |         |  |         |
/// | |     | |   | |     | |  | |     | |  |  No RG1 |  |  No RG1 |
/// | | RG1 | |   | | RG1 | |  | | RG1 | |  |         |  |         |
/// | |     | |   | |     | |  | |     | |  |         |  |         |
/// | +-----+ |   | +-----+ |  | +-----+ |  |         |  |         |
/// +---------+   +---------+  +---------+  +---------+  +---------+
///                New leader

class metadata_dissemination_service final
  : public ss::peering_sharded_service<metadata_dissemination_service> {
public:
    metadata_dissemination_service(
      ss::sharded<raft::group_manager>&,
      ss::sharded<cluster::partition_manager>&,
      ss::sharded<metadata_cache>&,
      ss::sharded<rpc::connection_cache>&);

    void disseminate_leadership(
      model::ntp, model::term_id, std::optional<model::node_id>);

    void initialize_leadership_metadata();

    ss::future<> start();
    ss::future<> stop();

private:
    // Used to store pending updates
    // When update was delivered successfully the finished flag is set to true
    // and object is removed from pending updates map
    struct update_retry_meta {
        ntp_leaders updates;
        bool finished = false;
    };
    // Used to track the process of requesting update when redpanda starts
    // when update using a node from ids will fail we will try the next one
    struct request_retry_meta {
        using container_t = std::vector<model::node_id>;
        using const_iterator = container_t::const_iterator;
        container_t ids;
        bool success = false;
        const_iterator next;
        exp_backoff_policy backoff_policy;
    };

    using broker_updates_t
      = absl::flat_hash_map<model::node_id, update_retry_meta>;

    void handle_leadership_notification(
      model::ntp, model::term_id, std::optional<model::node_id>);
    ss::future<> apply_leadership_notification(
      model::ntp, model::term_id, std::optional<model::node_id>);

    void collect_pending_updates();
    void cleanup_finished_updates();
    ss::future<> dispatch_disseminate_leadership();
    ss::future<> dispatch_one_update(model::node_id, update_retry_meta&);
    ss::future<result<get_leadership_reply>>
      dispatch_get_metadata_update(model::node_id);
    ss::future<> do_request_metadata_update(request_retry_meta&);
    ss::future<>
    process_get_update_reply(result<get_leadership_reply>, request_retry_meta&);

    ss::future<> update_metadata_with_retries(std::vector<model::node_id>);

    ss::sharded<raft::group_manager>& _raft_manager;
    ss::sharded<cluster::partition_manager>& _partition_manager;
    ss::sharded<metadata_cache>& _md_cache;
    ss::sharded<rpc::connection_cache>& _clients;
    model::node_id _self;
    std::chrono::milliseconds _dissemination_interval;
    std::vector<ntp_leader> _requests;
    std::vector<model::node_id> _seed_server_ids;
    broker_updates_t _pending_updates;
    mutex _lock;
    ss::timer<> _dispatch_timer;
    ss::abort_source _as;
    ss::gate _bg;
    cluster::notification_id_type _notification_handle;
};

} // namespace cluster
