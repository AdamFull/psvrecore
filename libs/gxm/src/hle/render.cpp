// Rendering a scene (a placeholder until the Vulkan path is in: the scene's draws are counted, nothing is drawn).

#include "psvrecore/log.hpp"
#include "state.hpp"

namespace psvrecore::gxm::hle {

void render_scene(Context& context, Scene& scene) {
    (void)context;
    nx::logd(logs::gxm, "scene: {} draws into 0x{:08x} ({}x{})", scene.draws.size(), scene.color.data,
             scene.color.size & 0xFFFF, scene.color.size >> 16);
}

}  // namespace psvrecore::gxm::hle
