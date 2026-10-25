package com.example.motonav;

/** Map content policy, independent of Android so every style can be tested. */
public final class MapStylePolicy {
    public static final int DRAGON_RADAR = 11;
    private MapStylePolicy() {}
    public static boolean isValid(int theme) { return theme >= 0 && theme <= DRAGON_RADAR; }
    public static boolean usesBasemap(int theme) { return theme != DRAGON_RADAR; }
    public static boolean usesAmapCapture(int theme) { return usesBasemap(theme) && theme != 10; }
}
