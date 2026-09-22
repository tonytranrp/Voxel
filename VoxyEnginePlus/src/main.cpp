#include "voxel-app.hpp"

#include <exception>
#include <iostream>

auto main() -> int
{
    // Unbuffered stdout so startup diagnostics survive a hard exit.
    setvbuf(stdout, nullptr, _IONBF, 0);

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
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "FATAL: unknown exception" << std::endl;
        return 1;
    }

    return 0;
}
