// Dust tiles: a magnetic sleeve for each ESP-Mosaico, and a wall plate that powers and holds them.
//
// The tiles touch. Each board sits in a thin sleeve with two small magnets per edge, set so any
// edge snaps to any other edge the right way round (N on the clockwise side of every edge, S on
// the other). The same magnets are what each tile's magnetometers sense to know its neighbours.
// Spring pins on the clockwise half of each edge and pads on the other half pass the 5.3 V bus
// from tile to tile, so pins always land on pads with + on +.
//
// Parts (each prints flat, no supports):
//   part = "sleeve"   one per tile (print 9)
//   part = "top"      wall plate top: spring contacts + holding magnets at all nine positions
//   part = "ring"     wall plate side ring with USB-C opening (holds the power electronics)
//   part = "cover"    wall plate back with keyholes, vents, standoffs and module cradle
//   part = "all"      preview
//
// MEASURE marks values to check against a real board. Run tools/magnet_check.py with the
// BMM150 positions you measure before printing sleeves.
//
//   openscad -D 'part="sleeve"' -o sleeve.stl dust_tiles.scad      (and top, ring, cover)

part = "all";

/* ---------- the board ---------- */
tile       = 46.0;   // MEASURE: board outline, square side
stack_h    = 9.0;    // MEASURE: BaseBoard + CoreBoard height, without the glass
clear      = 0.4;    // fit clearance per side

/* ---------- sleeve ---------- */
wall       = 2.0;    // tiles touch, so neighbouring screens sit 2 x wall + bezels apart
floor_t    = 1.2;
pitch      = tile + 2 * clear + 2 * wall;   // 50.8 mm with the defaults
lip        = 0.6;    // sleeve stands this far above the board stack
mag_d      = 3.15;   // 3 x 1 mm N42 disc, axis along the edge normal
mag_t      = 1.05;
mag_off    = 20.0;   // magnet distance from the edge centre (keeps them away from the sensors)
mag_z      = floor_t + 3.0;
pin_d      = 1.55;   // spring pogo pin barrel, through the wall
pad        = [2.4, 2.6, 0.6];   // pad recess (copper tape or a small PCB pad)
contact_z  = floor_t + 6.0;
contact_at = [-5.4, -1.8, 1.8, 5.4];   // along the edge, clockwise positive; + / - order below
back_pad   = [[-3, 11], [3, 11]];      // MEASURE: where the plate's spring pins meet the tile's back
washer     = [8.2, 1.0];               // steel washer recess (the plate's magnets pull on these)
washer_at  = [[0, 18], [0, -18]];

/* ---------- wall plate ---------- */
plate_w    = 3 * pitch + 6;
plate_t    = 4.0;
bay_d      = 22.0;   // room for the step-down module
ring_t     = 2.4;
cover_t    = 3.0;
corner_r   = 5.0;
plate_mag  = [3.15, 1.05];

$fn = 40;
H = floor_t + stack_h + lip;

module rsq(w, r) { offset(r) square(w - 2 * r, center = true); }

/* ---------- sleeve ---------- */
module sleeve() {
    difference() {
        union() {
            difference() {
                translate([-pitch/2, -pitch/2, 0]) cube([pitch, pitch, H]);
                translate([-(pitch/2 - wall), -(pitch/2 - wall), floor_t]) cube([pitch - 2 * wall, pitch - 2 * wall, H]);
            }
        }
        for (k = [0:3]) rotate(-90 * k) {          // k=0 top edge, then clockwise
            // edge magnets, flush with the outside face: N out at +mag_off (clockwise side), S out at -mag_off
            for (s = [1, -1]) translate([s * mag_off, pitch/2 - mag_t, mag_z]) rotate([-90, 0, 0]) cylinder(d = mag_d, h = mag_t + 0.01);
            // edge contacts: pins on the clockwise half, pads on the other half
            for (a = contact_at) translate([a, 0, contact_z])
                if (a > 0) translate([0, pitch/2 - wall - 0.01, 0]) rotate([-90, 0, 0]) cylinder(d = pin_d, h = wall + 0.02);
                else translate([-pad[0]/2, pitch/2 - pad[2], -pad[1]/2]) cube([pad[0], pad[2] + 0.01, pad[1]]);
            // wire channel from the contacts down to the floor
            translate([-6.5, pitch/2 - wall - 1.2, floor_t]) cube([13, 1.2, contact_z - floor_t + 1]);
        }
        // back: plate contact pads, washer recesses, and a pass-through for the bus wiring
        for (p = back_pad) translate([p[0], p[1], -0.01]) cylinder(d = 3.2, h = floor_t + 0.02);
        for (p = washer_at) translate([p[0], p[1], -0.01]) cylinder(d = washer[0], h = washer[1]);
        translate([-8, -pitch/2 + wall + 3, -0.01]) cube([16, 6, floor_t + 0.02]);   // tile bus board pocket
    }
}

/* ---------- wall plate ---------- */
function pos(i) = [((i % 3) - 1) * pitch, (1 - floor(i / 3)) * pitch];

module plate_top() {
    difference() {
        linear_extrude(plate_t) rsq(plate_w, corner_r);
        for (i = [0:8]) { p = pos(i);
            for (b = back_pad) translate([p[0] + b[0], p[1] + b[1], -0.01]) cylinder(d = pin_d, h = plate_t + 0.02);   // spring pins
            for (w = washer_at) translate([p[0] + w[0], p[1] + w[1], plate_t - plate_mag[1]]) cylinder(d = plate_mag[0], h = plate_mag[1] + 0.01);   // holding magnets
            translate([p[0] - 9, p[1] - 9, plate_t - 0.6]) linear_extrude(0.61) text(str(i + 1), size = 5, font = "Liberation Sans:style=Bold");
        }
        for (sx = [-1, 1], sy = [-1, 1]) translate([sx * (plate_w/2 - 5), sy * (plate_w/2 - 5), -0.01]) { cylinder(d = 3.4, h = plate_t + 1); translate([0, 0, plate_t - 2]) cylinder(d1 = 3.4, d2 = 6.4, h = 2.02); }
    }
}

module plate_ring() {
    difference() {
        union() {
            difference() { linear_extrude(bay_d) rsq(plate_w, corner_r); translate([0, 0, -1]) linear_extrude(bay_d + 2) rsq(plate_w - 2 * ring_t, corner_r - ring_t); }
            for (sx = [-1, 1], sy = [-1, 1]) translate([sx * (plate_w/2 - 5), sy * (plate_w/2 - 5), 0]) cylinder(d = 9, h = bay_d);
        }
        for (sx = [-1, 1], sy = [-1, 1]) translate([sx * (plate_w/2 - 5), sy * (plate_w/2 - 5), -1]) cylinder(d = 2.6, h = bay_d + 2);
        translate([0, -plate_w/2 + ring_t/2, 8]) cube([12, ring_t + 4, 6], center = true);   // USB-C
    }
}

module plate_cover() {
    difference() {
        union() {
            linear_extrude(cover_t) rsq(plate_w, corner_r);
            translate([0, 30, cover_t]) for (sx = [-1, 1], sy = [-1, 1]) translate([sx * 32.5, sy * 22.5, 0]) difference() { cylinder(d = 6, h = 6); translate([0, 0, 1]) cylinder(d = 2.2, h = 6); }
            translate([0, -28, cover_t]) for (sx = [-1, 1]) translate([sx * 32.2 - 1.2, -26, 0]) cube([2.4, 52, 4]);
        }
        for (sx = [-1, 1], sy = [-1, 1]) translate([sx * (plate_w/2 - 5), sy * (plate_w/2 - 5), -1]) { cylinder(d = 3.4, h = cover_t + 2); cylinder(d1 = 6.4, d2 = 3.4, h = 2); }
        for (sx = [-1, 1]) translate([sx * 50, plate_w/2 - 28, -1]) { cylinder(d = 8.5, h = cover_t + 2); hull() { cylinder(d = 4.2, h = cover_t + 2); translate([0, 9, 0]) cylinder(d = 4.2, h = cover_t + 2); } }
        for (k = [-3:3]) translate([k * 7, -28, -1]) hull() { translate([0, -18, 0]) cylinder(d = 3, h = cover_t + 2); translate([0, 18, 0]) cylinder(d = 3, h = cover_t + 2); }
    }
}

if (part == "sleeve") sleeve();
else if (part == "top") plate_top();
else if (part == "ring") plate_ring();
else if (part == "cover") plate_cover();
else {
    color("SlateGray") plate_cover();
    color("DimGray") translate([0, 0, cover_t]) plate_ring();
    color("Gainsboro") translate([0, 0, cover_t + bay_d]) plate_top();
    for (i = [0:8]) translate([pos(i)[0], pos(i)[1], cover_t + bay_d + plate_t + (i == 4 ? 18 : 0)]) color("LightSteelBlue") sleeve();
}

echo(str("Tile pitch ", pitch, " mm · sleeve height ", H, " mm · wall plate ", plate_w, " x ", plate_w, " x ", cover_t + bay_d + plate_t, " mm"));
