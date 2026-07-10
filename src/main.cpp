#include "ui/App.hpp"
#include <SDL.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    App app;
    if (!app.init()) return 1;
    app.run();
    return 0;
}
