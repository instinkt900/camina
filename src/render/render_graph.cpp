#include "render/render_graph.h"

#include "core/log.h"

#include <cstddef>
#include <iterator>

namespace engine::render {

    namespace {

        /// What a resource is in the middle of, as the walk goes along.
        struct Tracked {
            gfx::ResourceState state{}; ///< The state it is in now.
            bool written = false;       ///< True when the last access wrote it.
        };

        /// One access, after the reads and the writes of a pass are put together.
        struct Access {
            ResourceId resource;
            gfx::ResourceState state{};
            bool writes = false;
        };

        /**
         * Gathers the reads and the writes of one pass into one list.
         *
         * They are checked together rather than apart, because the conflict
         * worth catching is a resource named once as a read and once as a
         * write in two different states. Walking the two spans separately
         * would miss exactly that.
         */
        [[nodiscard]] bool gather(const PassDesc& pass, std::size_t at, std::size_t resource_count,
                                  std::vector<Access>& out) {
            out.clear();
            out.reserve(pass.reads.size() + pass.writes.size());
            for (const ResourceRead& read : pass.reads) {
                out.push_back(Access{ read.resource, read.state, false });
            }
            for (const ResourceWrite& write : pass.writes) {
                out.push_back(Access{ write.resource, write.state, true });
            }

            for (std::size_t i = 0; i < out.size(); ++i) {
                if (out[i].resource.index >= resource_count) {
                    ENGINE_LOG_ERROR("Pass {} (\"{}\") names resource {}, and there are only {}.",
                                     at, pass.name, out[i].resource.index, resource_count);
                    return false;
                }
                for (std::size_t j = i + 1; j < out.size(); ++j) {
                    if (out[i].resource != out[j].resource) {
                        continue;
                    }
                    if (out[i].state != out[j].state) {
                        ENGINE_LOG_ERROR("Pass {} (\"{}\") wants resource {} in state {} and in "
                                         "state {} at once. Declare it once.",
                                         at, pass.name, out[i].resource.index,
                                         gfx::resource_state_name(out[i].state),
                                         gfx::resource_state_name(out[j].state));
                        return false;
                    }
                    // The same resource in the same state twice is harmless.
                    // A pass that reads and writes one attachment says so, and
                    // one barrier covers both. Fold the pair into a write, so
                    // the ordering a write needs is not lost.
                    out[i].writes = out[i].writes || out[j].writes;
                    out.erase(out.begin() + static_cast<std::ptrdiff_t>(j));
                    --j;
                }
            }
            return true;
        }

    } // namespace

    bool derive_barriers(std::span<const PassDesc> passes,
                         std::span<const gfx::ResourceState> initial_states, GraphSchedule& out) {
        out.passes.clear();
        out.lifetimes.clear();
        out.final_states.clear();

        const std::size_t resource_count = initial_states.size();
        std::vector<Tracked> tracked(resource_count);
        for (std::size_t i = 0; i < resource_count; ++i) {
            tracked[i].state = initial_states[i];
            // Nothing has been written this frame, so the first access to a
            // resource is ordered against whatever left it in that state. The
            // caller says what that was by choosing the initial state.
            tracked[i].written = false;
        }

        std::vector<ResourceLifetime> lifetimes(resource_count);
        std::vector<PassBarriers> derived(passes.size());
        std::vector<Access> accesses;

        for (std::size_t at = 0; at < passes.size(); ++at) {
            const PassDesc& pass = passes[at];
            if (!gather(pass, at, resource_count, accesses)) {
                return false;
            }

            for (const Access& access : accesses) {
                const std::size_t index = access.resource.index;
                Tracked& now = tracked[index];

                // A write must be ordered against whatever touched the resource
                // before it, and a read must be ordered against a write. Only a
                // read that follows a read in the same state needs nothing.
                const bool needs = now.state != access.state || access.writes || now.written;
                if (needs) {
                    derived[at].before.push_back(
                        GraphBarrier{ access.resource, now.state, access.state });
                }

                now.state = access.state;
                now.written = access.writes;

                ResourceLifetime& life = lifetimes[index];
                if (!life.used) {
                    life.used = true;
                    life.first_pass = at;
                }
                life.last_pass = at;
            }
        }

        out.passes = std::move(derived);
        out.lifetimes = std::move(lifetimes);
        out.final_states.reserve(resource_count);
        for (const Tracked& entry : tracked) {
            out.final_states.push_back(entry.state);
        }
        return true;
    }

} // namespace engine::render
