#include "teckla_content.hpp"

const char *teckla_intro_text()
{
    return "Hello. I am Teckla, your Tesla Coil Klavier assistant. Play three white keys for the tutorial. Four white keys for Tesla coil physics. Any black key for the xstage story. One white and one black key for a joke. Press the lowest key to hear this menu again. Press the highest key to exit.";
}

const char *teckla_tutorial_text()
{
    return "Tutorial. You can keep playing while I talk. Every MIDI key makes a clean sine tone through the onboard speaker. The secret five white plus three black gesture opens this assistant menu.";
}

const char *teckla_physics_text()
{
    return "Tesla coils use resonance. Energy moves between an electric field in a capacitor and a magnetic field in an inductor. At the right frequency, voltage rises dramatically. This firmware is only an audio assistant and does not drive a coil.";
}

const char *teckla_xstage_text()
{
    return "The xstage idea is performance hardware as an instrument. In this build, the keyboard becomes a safe speaker based sketch pad: MIDI in, mixed tones and speech out, and no external actuators.";
}

const char *teckla_random_joke_text()
{
    return "Why did the capacitor join the band? Because it already knew how to charge the crowd.";
}

const char *teckla_goodbye_text()
{
    return "Goodbye. I will keep playing notes and wait quietly for the secret gesture.";
}
