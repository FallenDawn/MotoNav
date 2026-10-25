package com.example.motonav;

public final class MapStylePolicyTest {
    private static void require(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    public static void main(String[] args) {
        for (int theme = 0; theme <= 10; theme++) {
            require(MapStylePolicy.isValid(theme), "Original style rejected: " + theme);
            require(MapStylePolicy.usesBasemap(theme), "Original map disabled: " + theme);
            require(MapStylePolicy.usesAmapCapture(theme) == (theme != 10), "Original capture policy changed: " + theme);
        }
        int dragon = MapStylePolicy.DRAGON_RADAR;
        require(dragon == 11 && MapStylePolicy.isValid(dragon), "Wire ID must stay compatible with firmware");
        require(!MapStylePolicy.usesBasemap(dragon), "Radar must not fetch or transmit a basemap");
        require(!MapStylePolicy.usesAmapCapture(dragon), "Radar must not start a background map surface");
        require(!MapStylePolicy.isValid(-1) && !MapStylePolicy.isValid(12), "Invalid preferences must be rejected");
        System.out.println("PASS: all 12 styles, capture policy and invalid IDs");
    }
}
