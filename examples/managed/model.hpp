struct Device {
    int OUT = 5;
    int& IN = OUT;
    bool enabled = true;
    double gain = 1.5;
};

struct Monitor {
    int value;
};

struct Settings {
    int value = 12;
    int fallback = 6;
};

struct Controller {
    Settings settings;
    Controller() : settings{0} {}
};

Device A;
Device B{};
Monitor display;
Controller controller;

B.IN = A.OUT;
display.value = B.IN;
