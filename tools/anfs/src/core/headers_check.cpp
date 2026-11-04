// core/headers_check.cpp - compiles every frozen core and iface header together (ANFS-F0), so a header that stops
// being self-contained or consistent breaks the build of anfs_obj at once. Contains no code beyond static checks.
#include "core/clock.h"
#include "core/config.h"
#include "core/cors.h"
#include "core/db.h"
#include "core/fsutil.h"
#include "core/gate.h"
#include "core/http_client.h"
#include "core/http_ctx.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/log.h"
#include "core/mime.h"
#include "core/module.h"
#include "core/net.h"
#include "core/nodepath.h"
#include "core/status.h"
#include "core/stream.h"
#include "core/svc.h"
#include "core/version.h"
#include "core/zipstream.h"

#include "iface/agent_registry.h"
#include "iface/bridge_client.h"
#include "iface/browse_hooks.h"
#include "iface/bucket_store.h"
#include "iface/exec_runner.h"
#include "iface/index_store.h"
#include "iface/knowledge.h"
#include "iface/lan_only.h"
#include "iface/local_node.h"
#include "iface/media_passes.h"
#include "iface/mesh_auth.h"
#include "iface/node_store.h"
#include "iface/peer_client.h"
#include "iface/playplay_targets.h"
#include "iface/relay_hub.h"
#include "iface/tls_status.h"
#include "iface/transcripts.h"
#include "iface/transfer_io.h"
#include "iface/transfer_log.h"
#include "iface/transfer_registry.h"
#include "iface/watchdog_status.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace anfs::frozen_checks {

// JVal::type() casts the variant index: the alternative order is part of the frozen layout.
static_assert(static_cast<int>(JVal::Type::Null) == 0 && static_cast<int>(JVal::Type::Object) == 6, "JVal type order");
static_assert(std::is_nothrow_move_constructible_v<JVal>, "JVal moves without throwing (vectors of values)");
static_assert(std::is_copy_constructible_v<JVal> && std::is_copy_constructible_v<Config>, "value types copy");

// Services are interfaces: modules implement them, consumers only hold shared_ptrs from svcGet.
static_assert(std::is_abstract_v<IAgentRegistry> && std::is_abstract_v<INodeStore> && std::is_abstract_v<IRelayHub>);
static_assert(std::is_abstract_v<IPeerClient> && std::is_abstract_v<PeerResponse> && std::is_abstract_v<ILocalNode>);
static_assert(std::is_abstract_v<ITransferLog> && std::is_abstract_v<ITransferRegistry> && std::is_abstract_v<ITransferJob>);
static_assert(std::is_abstract_v<ITransferIo> && std::is_abstract_v<IBucketStore> && std::is_abstract_v<IIndexStore>);
static_assert(std::is_abstract_v<IBridgeClient> && std::is_abstract_v<IWatchdogStatus> && std::is_abstract_v<IBrowseHook>);
static_assert(std::is_abstract_v<IMediaPass> && std::is_abstract_v<ITranscriptSource> && std::is_abstract_v<ITranscripts>);
static_assert(std::is_abstract_v<ITranscribeFallback> && std::is_abstract_v<IKnowledge> && std::is_abstract_v<ITlsStatus>);
static_assert(std::is_abstract_v<IMeshAuthProvider> && std::is_abstract_v<IPlayplayTargets> && std::is_abstract_v<IExecRunner>);
static_assert(std::is_abstract_v<Module> && std::is_abstract_v<Clock> && std::is_abstract_v<Source> && std::is_abstract_v<Sink>);
static_assert(std::is_base_of_v<ITranscriptSource, ITranscripts>, "transcripts serve media-lib's source interface");
static_assert(std::is_base_of_v<Source, http::BodyReader> && std::is_base_of_v<Source, http::ClientResponse>);
static_assert(std::is_base_of_v<IoError, TooLarge> && std::is_base_of_v<IoError, http::ClientError>);

// Call sites, not just declarations: the natural argument types must pick one overload. An ambiguous call is a
// substitution failure inside decltype, so these detectors turn MSVC C2668 into a failed static_assert here.
template <typename T, typename = void> struct CanBind : std::false_type {};
template <typename T>
struct CanBind<T, std::void_t<decltype(std::declval<Stmt&>().bind(1, std::declval<T>()))>> : std::true_type {};
template <typename... T> constexpr bool canBindAll = (CanBind<T>::value && ...);
static_assert(canBindAll<int, unsigned, long, long long, int64_t, uint32_t, uint64_t, size_t, short, bool, char>,
              "Stmt::bind takes every integer type");
static_assert(canBindAll<double, float, std::string, const std::string&, std::string&, std::string_view, const char*,
                         const char (&)[4], char*, JVal, const JVal&, std::nullptr_t>,
              "Stmt::bind takes floating point, strings, literals, JVal and nullptr");

template <typename... T> constexpr bool toJValAll = (std::is_convertible_v<T, JVal> && ...);
static_assert(toJValAll<int, unsigned, long, long long, int64_t, uint32_t, uint64_t, size_t, short, bool, double, float,
                        std::string, const std::string&, std::string_view, const char*, const char (&)[4], std::nullptr_t,
                        JVal::Array, JObject>,
              "JVal converts implicitly and unambiguously from the natural value types");

template <typename T, typename = void> struct CanSource : std::false_type {};
template <typename T> struct CanSource<T, std::void_t<decltype(StringSource(std::declval<T>()))>> : std::true_type {};
static_assert(CanSource<std::string_view>::value && CanSource<std::string>::value && CanSource<const char (&)[4]>::value &&
              CanSource<const char*>::value && CanSource<std::string&>::value, "StringSource from views, temporaries, literals");
static_assert(!std::is_copy_constructible_v<StringSource> && !std::is_move_constructible_v<StringSource>,
              "StringSource may point into itself: never copied or moved");

static_assert(ExecRequest::clampTimeout(0) == 120000 && ExecRequest::clampTimeout(-5) == 1000 &&
              ExecRequest::clampTimeout(500) == 1000 && ExecRequest::clampTimeout(5000) == 5000 &&
              ExecRequest::clampTimeout(9999999) == 3600000, "Node's timeout clamp (agent.js:970)");

} // namespace anfs::frozen_checks
