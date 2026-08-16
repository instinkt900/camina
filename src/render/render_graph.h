#pragma once

/**
 * @file
 * @brief Turns what each pass declares into the barriers the frame needs.
 *
 * A pass says which resources it reads and which it writes, and in what state
 * it needs each one. This file takes a list of those declarations and works out
 * where a barrier has to go. Nothing here records a command, opens a device, or
 * names a Vulkan type.
 *
 * That is the whole reason the declaration is data rather than a builder a pass
 * calls into. The derivation is a pure function, so a test can drive it with no
 * GPU present. Barrier bugs appear on one vendor and no other, which makes them
 * the worst thing to leave untested. See DESIGN.md section 9 and issue #62.
 *
 * This is the first half of M5.3. It derives barriers and it does not alias
 * memory. Lifetimes are derived here anyway, because the aliasing in issue #122
 * needs them and they cost nothing to work out while the passes are being
 * walked.
 */

#include "gfx/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::render {

    /**
     * @brief Which resource a declaration means.
     *
     * An index into the state list handed to derive_barriers(). The graph owns
     * no resources and allocates nothing, so an index is the whole identity: a
     * caller keeps its own table and the graph only orders the work.
     */
    struct ResourceId {
        std::uint32_t index = 0; ///< The position in the state list.

        /// @brief Whether two ids name the same resource.
        /// @param a The first id.
        /// @param b The second id.
        /// @return True when the indices match.
        friend constexpr bool operator==(ResourceId a, ResourceId b) = default;
    };

    /**
     * @brief The swapchain image of a frame. Resource 0.
     *
     * This file names the resources a frame has, so a pass and the runtime
     * agree without passing a table around. A pass that owns a transient of its
     * own will need an id the caller hands it, and rule 4.6 says to build that
     * when a pass asks for it.
     *
     * Only the last pass of a frame writes this one. It is an 8-bit sRGB image,
     * so a value above 1 clips as it lands, and the scene therefore renders
     * into kSceneColor first.
     */
    inline constexpr ResourceId kFrameColor{ 0 };

    /// @brief The depth target of a frame. Resource 1.
    inline constexpr ResourceId kFrameDepth{ 1 };

    /**
     * @brief The directional shadow map. Resource 2.
     *
     * The first resource that is not a frame target. The shadow pass writes it
     * and the mesh pass reads it, which is the producer and consumer pair the
     * graph exists to order. It is also the first resource that keeps its
     * contents across the whole frame rather than being the frame itself.
     */
    inline constexpr ResourceId kShadowMap{ 2 };

    /**
     * @brief The half float image the scene renders into. Resource 3.
     *
     * The mesh pass writes it and the tonemap pass reads it, which is the
     * second producer and consumer pair in the frame. It exists because the
     * swapchain image cannot hold a value above 1.
     *
     * Like the shadow map, one image serves every frame in flight, so its state
     * carries from one frame into the next rather than starting at
     * ResourceState::Undefined. Resetting it would derive a barrier that waits
     * on the color attachment stage, and what it has to wait for is the
     * previous frame's fragment shader reading it. That is a write after read,
     * and it is the same hazard #125 found on the shared depth image.
     */
    inline constexpr ResourceId kSceneColor{ 3 };

    /**
     * @brief The per-cell light list of the cluster grid. Resource 4.
     *
     * The first resource that is a buffer rather than an image. The cull pass
     * writes it and the mesh pass reads it. Nothing about the derivation cares
     * which kind it is, because a ResourceId is only an index and the states
     * are the whole vocabulary. The caller issuing the barriers is what knows,
     * and a buffer needs no layout.
     *
     * Its state carries across frames for the reason kSceneColor gives. The
     * barrier that falls out at the top of a frame orders this frame's compute
     * writes against the last frame's fragment reads.
     */
    inline constexpr ResourceId kClusterGrid{ 4 };

    /**
     * @brief The picture an editor draws into a panel. Resource 5.
     *
     * The tonemap pass writes the swapchain image when a program fills the
     * window with the scene, and it writes this one when the scene lives inside
     * a panel. Then the overlay reads it and writes the swapchain, which is one
     * more producer and consumer pair.
     *
     * A frame uses one or the other and never both. A runtime frame never names
     * this, so its state stays where it started and no barrier is derived for
     * it.
     *
     * Its state carries across frames for the reason kSceneColor gives.
     */
    inline constexpr ResourceId kViewportColor{ 5 };

    /// @brief How many resources a frame declares, which is the length of the
    /// state list derive_barriers() takes.
    inline constexpr std::uint32_t kFrameResourceCount = 6;

    /**
     * @brief One resource a pass reads.
     *
     * A read never changes what a resource holds, so several passes in a row
     * that read one resource in the same state need no barrier between them.
     */
    struct ResourceRead {
        ResourceId resource;        ///< What to read.
        gfx::ResourceState state{}; ///< The state the pass needs it in.
    };

    /**
     * @brief One resource a pass writes.
     *
     * Separate from ResourceRead rather than one shared type with a flag. The
     * two are the same shape today and they do not stay that way: a write is
     * where a discard hint belongs, and a read has nothing to discard. Keeping
     * them apart also stops a caller putting one where the other goes, which
     * the compiler could not catch if they were one type.
     */
    struct ResourceWrite {
        ResourceId resource;        ///< What to write.
        gfx::ResourceState state{}; ///< The state the pass needs it in.
    };

    /**
     * @brief What one pass reads and writes.
     *
     * The spans point at storage the caller owns, and they must outlive the
     * derive_barriers() call. A pass usually returns this from a `declare()`
     * method over members that live as long as the pass does.
     */
    struct PassDesc {
        /// @brief A name for the log. It appears in every message about this pass.
        std::string_view name;
        std::span<const ResourceRead> reads;   ///< Every resource it reads.
        std::span<const ResourceWrite> writes; ///< Every resource it writes.
    };

    /**
     * @brief One state change a resource has to make before a pass runs.
     *
     * A barrier where @c before equals @c after is not a mistake. Two writes to
     * one resource in the same state still have to be ordered against each
     * other, and that ordering is the only thing such a barrier carries.
     */
    struct GraphBarrier {
        ResourceId resource;         ///< What changes.
        gfx::ResourceState before{}; ///< The state it is in now.
        gfx::ResourceState after{};  ///< The state the pass needs.
    };

    /// @brief The barriers to issue before one pass records.
    struct PassBarriers {
        std::vector<GraphBarrier> before; ///< In the order they were derived.
    };

    /**
     * @brief The span of passes over which a resource is live.
     *
     * The aliasing in issue #122 reads this. Two resources whose lifetimes do
     * not overlap can share one allocation, and this is what says so.
     */
    struct ResourceLifetime {
        std::size_t first_pass = 0; ///< The first pass that touches it.
        std::size_t last_pass = 0;  ///< The last pass that touches it.
        bool used = false;          ///< False when no pass named it at all.
    };

    /**
     * @brief What derive_barriers() worked out for one frame.
     */
    struct GraphSchedule {
        /// @brief One entry for each pass, in the order they were given.
        std::vector<PassBarriers> passes;
        /// @brief One entry for each resource, indexed by ResourceId::index.
        std::vector<ResourceLifetime> lifetimes;
        /// @brief The state each resource is left in after the last pass.
        std::vector<gfx::ResourceState> final_states;
    };

    /**
     * @brief Works out every barrier a list of passes needs.
     *
     * The passes run in the order given. This walks them once, tracks what
     * state each resource is in, and records a barrier wherever a pass needs a
     * resource in a state it is not already in, or wherever two accesses have
     * to be ordered against each other.
     *
     * A barrier is recorded when any of these is true:
     *
     * - The resource is not already in the state the pass needs.
     * - The pass writes it. Two writes must be ordered, and so must a write
     *   that follows a read.
     * - The last pass to touch it wrote it. A read cannot start before the
     *   write it follows has finished.
     *
     * So a read that follows a read in the same state is the one case that
     * needs nothing, which is the case worth getting right: it is the common
     * one and a barrier there costs real time for nothing.
     *
     * @param passes The passes, in the order they run.
     * @param initial_states The state each resource starts the frame in. Its
     * length is how many resources there are, and a ResourceId indexes it.
     * @param out The result. Cleared first, and left empty when this fails.
     * @return True when every declaration was valid. On false the log names the
     * pass and the resource, and @p out holds nothing.
     *
     * @warning A pass that names one resource twice with two different states
     * is refused rather than resolved. There is no answer to what a pass wants
     * a resource to be in two states at once, and picking one would hide the
     * mistake.
     *
     * @code
     * const std::array reads{ engine::render::ResourceRead{ shadow_map,
     *                                                       gfx::ResourceState::ShaderRead } };
     * const std::array writes{ engine::render::ResourceWrite{ backbuffer,
     *                                                         gfx::ResourceState::ColorTarget } };
     * const std::array passes{ engine::render::PassDesc{ "mesh", reads, writes } };
     * engine::render::GraphSchedule schedule;
     * if (engine::render::derive_barriers(passes, initial, schedule)) {
     *     // schedule.passes[0].before holds what to issue before the mesh pass.
     * }
     * @endcode
     */
    [[nodiscard]] bool derive_barriers(std::span<const PassDesc> passes,
                                       std::span<const gfx::ResourceState> initial_states,
                                       GraphSchedule& out);

} // namespace engine::render
