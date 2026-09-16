#define OLC_PGE3_APPLICATION

#include "000_platform_fix.hpp"
#include "010_types.hpp"
#include "020_color_utils.hpp"
#include "030_thread_pool.hpp"
#include "040_file_watcher.hpp"
#include "050_treemap_layout.hpp"
#include "060_scanner.hpp"
#include "070_treemap_app.hpp"
#include "080_cli.hpp"

int main(int argc, char* argv[])
{
    const auto options = pge_treemap::ParseCli(argc, argv);
    if (!options.has_value())
    {
        return 0;
    }

    const PGEConfig config = [] {
        PGEConfig c{
            .vScreenSize = {1920, 1080},
            .vPixelSize  = {1, 1},
            .bVSync      = true};
        c.bFullScreen = false;
        return c;
    }();

    if (pge_treemap::DiskTreemapAnalyzer demo(options->targetDir); demo.Construct(config))
    {
        demo.Start();
    }

    return 0;
}
