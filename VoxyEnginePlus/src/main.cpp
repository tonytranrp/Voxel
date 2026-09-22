#include "voxel-app.hpp"

#include <exception>
#include <iostream>
#include <logger.hpp>

auto main() -> int
{
    // Unbuffered stdout so startup diagnostics survive a hard exit, and a log file so a packaged
    // build that fails has something to show for it after its window closes.
    setvbuf(stdout, nullptr, _IONBF, 0);
    Logger::Init();

    std::cout << "Voxy starting up\n";

    int exit_code = 0;
    try
    {
        VoxyApp app;
        while (true)
        {
            if (app.Update())
            {
                break;
            }
        }
    }
    catch (std::exception const &e)
    {
        std::cerr << "\n================================ FAILED ================================\n"
                  << e.what()
                  << "\n\nA log has been written to voxy-log.txt next to the executable.\n"
                     "If you are reporting this, please send that file.\n"
                     "=======================================================================\n";
        exit_code = 1;
    }
    catch (...)
    {
        std::cerr << "\n================================ FAILED ================================\n"
                     "Unknown error.\n\nA log has been written to voxy-log.txt next to the executable.\n"
                     "=======================================================================\n";
        exit_code = 1;
    }

    std::cout.flush();
    std::cerr.flush();
    Logger::Shutdown();
    return exit_code;
}
