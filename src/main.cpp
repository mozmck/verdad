#include "app/VerdadApp.h"

#include <FL/Fl.H>
#include <FL/Fl_File_Icon.H>

#include <iostream>
#include <cstdlib>


int main(int argc, char* argv[]) {
    verdad::VerdadApp app;
    
	//FL_NORMAL_SIZE = 12;
	//Fl::scheme("gtk+");
	//Fl::option(Fl::OPTION_FNFC_USES_GTK, true);
	//Fl::add_handler(handle);    //handle system keys (ESC in particular)
	Fl_File_Icon::load_system_icons();


    if (!app.initialize(argc, argv)) {
        std::cerr << "Failed to initialize Verdad." << std::endl;
        return EXIT_FAILURE;
    }

    return app.run();
}
