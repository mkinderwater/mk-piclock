// Compact OLED + Raspberry Pi Zero case
// Speaker update:
// OLED/SPI runs directly from Raspberry Pi Zero GPIO; no level shifter/value shifter board or mounts required.
// 32mm x 29mm speaker pocket kept clear so no baffle/support wall hits the speaker
// 31mm wide x 28mm tall speaker footprint
// vertical speaker mount tabs integrated into the baffle, no round end pads
// 36.5mm mount centre spacing gives 5.85mm clearance from each mount edge to the inside wall
// speaker mount tabs and baffle are 7.0mm high from the inside base floor
// 28mm slotted speaker grille, centered in 31mm x 28mm speaker footprint
// speaker mounts, grille, baffle shifted 3mm left toward the wall
// speaker centered vertically so both speaker mounts have 5.85mm clearance to the inside walls
// printed speaker acoustic baffle added, 1.5mm wall, no rubber seal required
// Pi on top, 20x19 amp board on bottom; amp standoffs shifted 12mm right from left Pi mounts; Pi gussets point inward, amp gussets point right
// USB-C power header floor flush mount on speaker grille side; 12.2mm x 7mm header cutout; bottom-right, 10mm from inside walls; no wall cutouts
// USB-C recess increased by 0.5mm
// MicroSD slot reduced by 4mm on one side, plus 1mm from the foot-side edge, plus 1mm from the HDMI-facing edge
// 4mm centered speaker wire relief added on the right side of the speaker baffle, 50% baffle height
// Speaker wire relief reduced to 50% baffle height
// Lid supports extended 30% inward so screw pilot is not at the support end
// Blind lid support pilot holes added; holes stop short and do not cut through the wall
// Speaker mount support tabs are squared off and 7.0mm high from the inside base floor
// Base part number raised inside under the Pi mount
// USB-C polarity mark raised on inside floor below cutout: + only, moved 1mm lower
// Units: mm

$fn = 64;

part = "base"; // "base", "lid", "assembly"

fit_fudge = 0.3;

// Case
case_w = 114;
case_h = 60;
case_d = 50;

wall = 3;
corner_r = 4;

lid_t = 3;
lip_depth = 6;
lip_clearance = 0.35;
lip_wall = 2.2;

assembly_lid_lift = 8;

// OLED module
oled_pcb_w = 100.5;
oled_pcb_h = 33.5;

// Screen opening
oled_screen_w = 81.5;
oled_screen_h = 24.0;

oled_view_w = 76.7;
oled_view_h = 19.2;

oled_pcb_t = 1.6;

oled_hole_span_x = 95;
oled_hole_span_y = 28.5;

oled_x = (case_w - oled_pcb_w) / 2;
oled_y = (case_h - oled_pcb_h) / 2;

oled_screen_x = oled_x + (oled_pcb_w - oled_screen_w) / 2;
oled_screen_y = oled_y + (oled_pcb_h - oled_screen_h) / 2 + 2;

oled_view_x = oled_x + (oled_pcb_w - oled_view_w) / 2;
oled_view_y = oled_y + (oled_pcb_h - oled_view_h) / 2 + 2;

oled_hole_x_offset = (oled_pcb_w - oled_hole_span_x) / 2;
oled_hole_y_offset = (oled_pcb_h - oled_hole_span_y) / 2;

// M3 through holes for OLED mounting
oled_m3_clearance_d = 3.0 + fit_fudge;
oled_m3_head_preview_d = 5.8;
oled_m3_washer_preview_d = 7.0;
oled_m3_nut_preview_d = 6.4;

// 3.0mm spacer/padding between lid underside and OLED PCB
oled_solder_joint_padding = 3.0;
oled_solder_padding_d = 6.4;

// Raspberry Pi Zero
pi_w = 65;
pi_h = 30;
pi_corner_r = 3;

pi_x = case_w - wall - 2 - pi_w;

// Vertical board stack: amp on bottom, Pi on top.
board_gap = 2;
pi_shift_y = -1.5;        // move Pi 1.5mm down toward the amp
amp_w = 20;
amp_h = 19;
amp_corner_r = 2;
amp_hole_span = 12.5;
amp_y = wall + 2;
pi_y = amp_y + amp_h + board_gap + pi_shift_y;

pi_holes = [
    [3.5, 3.5],
    [61.5, 3.5],
    [3.5, 26.5],
    [61.5, 26.5]
];

// M2 Pi standoffs
pi_standoff_h = 6;
pi_standoff_od = 5.8;
pi_standoff_pilot_d = 1.8;

// Pi standoff gussets
pi_gusset_h = 4.5;
pi_gusset_w = 1.6;
pi_gusset_len = 6;

// 20x19 amplifier board, below the Pi.
// Standoff OD and pilot match the Pi standoffs.
// Amp standoffs are 12.5mm apart on the Y axis.
// Amp standoffs and amp preview are shifted 12mm right from the left Pi standoff column.
amp_shift_x = 12;
amp_standoff_h = pi_standoff_h;
amp_standoff_od = pi_standoff_od;
amp_standoff_pilot_d = pi_standoff_pilot_d;

amp_hole_x = pi_x + 3.5 + amp_shift_x;
amp_x = amp_hole_x - amp_w / 2;

amp_holes = [
    [amp_w / 2, amp_h / 2 - amp_hole_span / 2],
    [amp_w / 2, amp_h / 2 + amp_hole_span / 2]
];

amp_lower_hole_y = amp_y + amp_h / 2 - amp_hole_span / 2;
amp_upper_hole_y = amp_y + amp_h / 2 + amp_hole_span / 2;

// Speaker
// Physical speaker footprint after it is mounted into the holes.
speaker_w = 31;
speaker_h = 28;

// Speaker is centered between the left wall and the Pi, then shifted left 3mm.
// Vertically centered so both speaker mount cylinders clear the inside case walls by 5.85mm.
speaker_shift_x = -3;
speaker_shift_y = 0;
speaker_center_x = wall + (pi_x - wall) / 2 + speaker_shift_x;
speaker_center_y = case_h / 2 + speaker_shift_y;

// Speaker mount specs
speaker_mount_nominal_h = 7;
speaker_baffle_top_clearance = 0.0;
speaker_baffle_h = speaker_mount_nominal_h; // 7.0mm from inside base floor
speaker_mount_h = speaker_baffle_h; // actual integrated mount tab height, flush with baffle
speaker_mount_w = pi_standoff_od;
speaker_mount_pilot_d = pi_standoff_pilot_d;
speaker_mount_span = 36.5; // center-to-center distance between speaker mount pilot holes
speaker_mount_center_to_wall = (case_h - 2 * wall - speaker_mount_span) / 2; // 8.75mm from inside wall to mount center
speaker_mount_wall_clearance = speaker_mount_center_to_wall - speaker_mount_w / 2; // 5.85mm gap from mount tab edge to inside wall

speaker_mount_support_w = speaker_mount_w;
speaker_mount_support_h = speaker_baffle_h;
// Wall-bridged speaker mount tabs are kept in the top/bottom walls.
// The baffle-side ends are squared off and blend into the speaker baffle instead of ending round.
// No separate round speaker mount cylinders are added. The pilot holes are cut into the flat tabs.
speaker_support_wall_overlap = 0.8;

// Slotted speaker grille, kept inside a 28mm footprint.
speaker_grill_d = 28;
speaker_slot_w = 2.2;
speaker_slot_h = 22.0;
speaker_slot_gap = 1.25;
speaker_slot_count = 7;

// Printed acoustic baffle around speaker front.
// No rubber/foam seal is assumed.
// Baffle and integrated mount tabs share the same height.
// Rectangular to match the 31mm x 28mm seated speaker.
// IMPORTANT: the speaker pocket is a hard keepout. Nothing may enter this area.
// Added 0.5mm clearance per side between speaker and baffle walls.
// Outer baffle size is kept the same by reducing the baffle wall from 1.5mm to 1.4mm.
speaker_baffle_wall = 1.4;
speaker_pocket_clearance = 1.0; // total clearance added to the 31mm x 28mm speaker pocket, 0.5mm per side
speaker_pocket_w = speaker_w + speaker_pocket_clearance;
speaker_pocket_h = speaker_h + speaker_pocket_clearance;
speaker_baffle_outer_w = speaker_pocket_w + speaker_baffle_wall * 2;
speaker_baffle_outer_h = speaker_pocket_h + speaker_baffle_wall * 2;
speaker_baffle_inner_w = speaker_pocket_w;
speaker_baffle_inner_h = speaker_pocket_h;
speaker_baffle_corner_r = 2.0;

// Right-side speaker wire relief.
// Wires exit at the middle of the speaker's right side.
// Relief is only the upper 50% of the baffle height to preserve the lower acoustic seal.
speaker_wire_relief_w = 4.0;
speaker_wire_relief_depth = speaker_baffle_wall + 0.8;
speaker_wire_relief_h = speaker_baffle_h * 0.5;

// USB positions retained for preview only
// No USB case cutout is used.
usb_cutout_w = 10.5;
usb_cutout_h = 10;
usb_cutout_z = wall + pi_standoff_h + 1 - 2.5;

usb_centers_x = [
    pi_x + (pi_w - 54.0),
    pi_x + (pi_w - 41.4)
];

usb_side_pad = 8;

usb_slot_x =
    min(usb_centers_x[0], usb_centers_x[1])
    - usb_cutout_w / 2
    - usb_side_pad;

usb_slot_w =
    max(usb_centers_x[0], usb_centers_x[1])
    - min(usb_centers_x[0], usb_centers_x[1])
    + usb_cutout_w
    + usb_side_pad * 2;

// Micro SD access slot on right wall
// Tied to pi_y, so it moves with the Pi.
// Original 18mm slot, reduced by 4mm on one side,
// then reduced another 1mm from the foot-side edge,
// then reduced 1mm from the HDMI-facing edge.
sd_slot_w_original = 18;
sd_slot_trim_right = 4;
sd_slot_trim_foot_side = 1;
sd_slot_trim_hdmi_side = 1;
sd_slot_w = sd_slot_w_original - sd_slot_trim_right - sd_slot_trim_foot_side - sd_slot_trim_hdmi_side;
sd_slot_h = 6;
sd_slot_z = wall + pi_standoff_h;
sd_slot_y = pi_y + (pi_h / 2) - (sd_slot_w_original / 2) + sd_slot_trim_foot_side;

// USB-C power header floor flush mount
// Footprint: 20mm wide x 7mm tall x 2.1mm board depth
// Header body cutout: 12.2mm x 7mm through the speaker grille side of the base
// Hole spacing: 16mm on X, centered on the 20mm width
// Mounted into the same bottom face as the speaker grille, with no side wall cutouts.
// Positioned bottom-right, with a 10mm gap from the inside right and bottom walls.
// The module is recessed 2.2mm from the outside bottom face so the board sits deeper.
usb_c_panel_w = 20;
usb_c_panel_h = 7;
usb_c_panel_depth = 2.2;
usb_c_header_cutout_w = 12.2;
usb_c_header_cutout_h = 7;
usb_c_hole_span = 16;
usb_c_mount_hole_d = 2.4;
usb_c_mount_pilot_d = pi_standoff_pilot_d;

usb_c_recess_fudge = 0.25;
usb_c_clearance_from_walls = 10.0;

usb_c_panel_x = case_w - wall - usb_c_clearance_from_walls - usb_c_panel_w;
usb_c_panel_y = wall + usb_c_clearance_from_walls;
usb_c_panel_center_x = usb_c_panel_x + usb_c_panel_w / 2;
usb_c_panel_center_y = usb_c_panel_y + usb_c_panel_h / 2;
usb_c_panel_z = 0;

usb_c_hole_centers = [
    [usb_c_panel_center_x - usb_c_hole_span / 2, usb_c_panel_center_y],
    [usb_c_panel_center_x + usb_c_hole_span / 2, usb_c_panel_center_y]
];

// Raised support on the inside floor, beside the recessed USB-C plate.
// Height is 5mm to provide screw support for the USB-C insert.
usb_c_support_rail_w = 1.4;
usb_c_support_rail_h = 5.0;
usb_c_support_end_w = 1.4;

// Preview only: shows the USB-C header body sticking upward through the 12.2mm x 7mm base cutout.
usb_c_connector_preview_w = usb_c_header_cutout_w;
usb_c_connector_preview_d = usb_c_header_cutout_h;
usb_c_connector_preview_h = 6.0;

// Base part number, raised on the inside floor under the Pi mount.
base_part_number = "MK-PiClock-V2.21";
base_part_number_size = 4.0;
base_part_number_h = 0.45;
base_part_number_x = pi_x + pi_w / 2;
base_part_number_y = pi_y + pi_h / 2;

// USB-C polarity marking, raised on the inside floor below the USB-C cutout.
usb_c_polarity_mark_size = 4.0;
usb_c_polarity_mark_h = 0.45;
usb_c_polarity_mark_y = usb_c_panel_y - 4.0;
usb_c_polarity_plus_x = usb_c_panel_center_x - usb_c_hole_span / 2;

// Bottom wall rubber foot recesses
rubber_foot_size = 22;
rubber_foot_recess_depth = 1.2;
rubber_foot_x_margin = 8;
rubber_foot_z_margin = 2;

rubber_foot_x_positions = [
    rubber_foot_x_margin,
    case_w - rubber_foot_x_margin - rubber_foot_size
];

rubber_foot_z_positions = [
    rubber_foot_z_margin,
    case_d - rubber_foot_z_margin - rubber_foot_size
];

// Micro HDMI remains covered
micro_hdmi_center_x = pi_x + (pi_w - 12.4);

// Lid screws
lid_screw_margin = 7;
lid_screw_d = 3.0;
lid_screw_head_d = 6.8;
lid_screw_head_recess_h = 1.8;

// Base sloped lid mounts
lid_mount_pilot_d = 2.65;
lid_mount_pilot_entry_d = 3.05;
lid_mount_pilot_entry_h = 2.2;
lid_mount_pilot_bottom_solid = 5.0;

lid_mount_top_h = 3.4;
lid_mount_grow_h = 14.4;
lid_mount_h = lid_mount_grow_h + lid_mount_top_h;

lid_mount_base_reach = 1.2;
lid_mount_wall_overlap = 0.55;
lid_mount_inner_extra = 3.5;
// Extend the lid support body 30% beyond the screw pilot location.
// This keeps the pilot from being at the end/tip of the support.
lid_mount_reach_scale = 1.30;
lid_mount_lip_clearance = 1.2;
lid_mount_hull_slice_h = 0.25;

// Lid mount holes are blind. They start at the top of the support and stop short,
// leaving solid material so the holes do not break through the wall/support.
lid_mount_blind_pilot_depth = 10.0;

function lid_points() = [
    [lid_screw_margin, lid_screw_margin],
    [case_w - lid_screw_margin, lid_screw_margin],
    [lid_screw_margin, case_h - lid_screw_margin],
    [case_w - lid_screw_margin, case_h - lid_screw_margin]
];

module rounded_cube(size, r) {
    hull() {
        translate([r, r, 0]) cylinder(h = size[2], r = r);
        translate([size[0] - r, r, 0]) cylinder(h = size[2], r = r);
        translate([r, size[1] - r, 0]) cylinder(h = size[2], r = r);
        translate([size[0] - r, size[1] - r, 0]) cylinder(h = size[2], r = r);
    }
}

module oled_holes() {
    translate([oled_x + oled_hole_x_offset, oled_y + oled_hole_y_offset, 0]) children();
    translate([oled_x + oled_pcb_w - oled_hole_x_offset, oled_y + oled_hole_y_offset, 0]) children();
    translate([oled_x + oled_hole_x_offset, oled_y + oled_pcb_h - oled_hole_y_offset, 0]) children();
    translate([oled_x + oled_pcb_w - oled_hole_x_offset, oled_y + oled_pcb_h - oled_hole_y_offset, 0]) children();
}

module oled_screen_cutout() {
    translate([
        oled_screen_x - fit_fudge / 2,
        oled_screen_y - fit_fudge / 2,
        -1
    ])
        cube([
            oled_screen_w + fit_fudge,
            oled_screen_h + fit_fudge,
            lid_t + 2
        ]);
}

module oled_m3_lid_holes() {
    oled_holes() {
        translate([0, 0, -oled_solder_joint_padding - 1])
            cylinder(
                h = lid_t + oled_solder_joint_padding + 2,
                d = oled_m3_clearance_d
            );
    }
}

module oled_half_moon_padding_pad(keep_angle) {
    intersection() {
        translate([0, 0, -oled_solder_joint_padding])
            cylinder(
                h = oled_solder_joint_padding,
                d = oled_solder_padding_d
            );

        rotate([0, 0, keep_angle])
            translate([
                0,
                -oled_solder_padding_d / 2,
                -oled_solder_joint_padding - 0.05
            ])
                cube([
                    oled_solder_padding_d / 2,
                    oled_solder_padding_d,
                    oled_solder_joint_padding + 0.1
                ]);
    }
}

module oled_solder_padding_pads() {
    oled_screen_cx = oled_screen_x + oled_screen_w / 2;
    oled_screen_cy = oled_screen_y + oled_screen_h / 2;

    ll_x = oled_x + oled_hole_x_offset;
    lr_x = oled_x + oled_pcb_w - oled_hole_x_offset;
    ly = oled_y + oled_hole_y_offset;
    uy = oled_y + oled_pcb_h - oled_hole_y_offset;

    translate([ll_x, ly, 0])
        oled_half_moon_padding_pad(atan2(ly - oled_screen_cy, ll_x - oled_screen_cx));

    translate([lr_x, ly, 0])
        oled_half_moon_padding_pad(atan2(ly - oled_screen_cy, lr_x - oled_screen_cx));

    translate([ll_x, uy, 0])
        oled_half_moon_padding_pad(atan2(uy - oled_screen_cy, ll_x - oled_screen_cx));

    translate([lr_x, uy, 0])
        oled_half_moon_padding_pad(atan2(uy - oled_screen_cy, lr_x - oled_screen_cx));
}

module lid_lip_ring() {
    difference() {
        translate([wall + lip_clearance, wall + lip_clearance, -lip_depth])
            rounded_cube(
                [
                    case_w - 2 * (wall + lip_clearance),
                    case_h - 2 * (wall + lip_clearance),
                    lip_depth
                ],
                max(corner_r - wall, 0.5)
            );

        translate([
            wall + lip_clearance + lip_wall,
            wall + lip_clearance + lip_wall,
            -lip_depth - 0.1
        ])
            rounded_cube(
                [
                    case_w - 2 * (wall + lip_clearance + lip_wall),
                    case_h - 2 * (wall + lip_clearance + lip_wall),
                    lip_depth + 0.2
                ],
                max(corner_r - wall - lip_wall, 0.5)
            );
    }
}

module pi_mount_holes() {
    for (p = pi_holes) {
        translate([pi_x + p[0], pi_y + p[1], 0]) children();
    }
}

module amp_mount_holes() {
    for (p = amp_holes) {
        translate([amp_x + p[0], amp_y + p[1], 0]) children();
    }
}

module pi_standoff_gusset_at(x, y, angle) {
    translate([x, y, wall])
        rotate([0, 0, angle])
            hull() {
                translate([0, -pi_gusset_w / 2, 0])
                    cube([0.1, pi_gusset_w, pi_gusset_h]);

                translate([pi_gusset_len, -pi_gusset_w / 2, 0])
                    cube([0.1, pi_gusset_w, 0.1]);
            }
}

module pi_standoff_gussets() {
    // Aim each Pi gusset inward toward the Pi board interior.
    pi_standoff_gusset_at(pi_x + 3.5,  pi_y + 3.5,  45);
    pi_standoff_gusset_at(pi_x + 61.5, pi_y + 3.5,  135);
    pi_standoff_gusset_at(pi_x + 3.5,  pi_y + 26.5, 315);
    pi_standoff_gusset_at(pi_x + 61.5, pi_y + 26.5, 225);
}

module amp_standoff_gussets() {
    // Aim both amp gussets right, toward the Pi/body area.
    pi_standoff_gusset_at(amp_hole_x, amp_lower_hole_y, 0);
    pi_standoff_gusset_at(amp_hole_x, amp_upper_hole_y, 0);
}

module speaker_mount_points() {
    translate([
        speaker_center_x,
        speaker_center_y - speaker_mount_span / 2,
        0
    ])
        children();

    translate([
        speaker_center_x,
        speaker_center_y + speaker_mount_span / 2,
        0
    ])
        children();
}

module speaker_support_arm_to_bottom_wall(x1, y1, x2) {
    // Flat wall-bridged tab. The baffle-side end is no longer round.
    // It overlaps the lower baffle wall but stops before the speaker pocket keepout.
    tab_y0 = wall - speaker_support_wall_overlap;
    tab_y1 = speaker_center_y - speaker_baffle_inner_h / 2 - 0.15;

    translate([
        x2 - speaker_mount_support_w / 2,
        tab_y0,
        wall
    ])
        cube([
            speaker_mount_support_w,
            tab_y1 - tab_y0,
            speaker_mount_support_h
        ]);
}

module speaker_support_arm_to_top_wall(x1, y1, x2) {
    // Flat wall-bridged tab. The baffle-side end is no longer round.
    // It overlaps the upper baffle wall but stops before the speaker pocket keepout.
    tab_y0 = speaker_center_y + speaker_baffle_inner_h / 2 + 0.15;
    tab_y1 = case_h - wall + speaker_support_wall_overlap;

    translate([
        x2 - speaker_mount_support_w / 2,
        tab_y0,
        wall
    ])
        cube([
            speaker_mount_support_w,
            tab_y1 - tab_y0,
            speaker_mount_support_h
        ]);
}

module speaker_mount_supports_raw() {
    lower_x = speaker_center_x;
    lower_y = speaker_center_y - speaker_mount_span / 2;

    upper_x = speaker_center_x;
    upper_y = speaker_center_y + speaker_mount_span / 2;

    speaker_support_arm_to_bottom_wall(
        lower_x,
        lower_y,
        lower_x
    );

    speaker_support_arm_to_top_wall(
        upper_x,
        upper_y,
        upper_x
    );
}

module speaker_mount_supports() {
    speaker_mount_supports_raw();
}

module speaker_pocket_keepout() {
    // Keep the full 31mm x 28mm speaker pocket plus clearance free of walls,
    // ribs, baffle material, and support arms. Starts above the grille floor.
    translate([
        speaker_center_x - speaker_pocket_w / 2,
        speaker_center_y - speaker_pocket_h / 2,
        wall - 0.05
    ])
        rounded_cube([
            speaker_pocket_w,
            speaker_pocket_h,
            speaker_mount_h + 1.0
        ], max(speaker_baffle_corner_r - 0.7, 0.5));
}

module rounded_slot_cutout(w, h, depth) {
    hull() {
        translate([0, -h / 2 + w / 2, 0])
            cylinder(h = depth, d = w);

        translate([0, h / 2 - w / 2, 0])
            cylinder(h = depth, d = w);
    }
}

module speaker_grill_holes() {
    total_w = speaker_slot_count * speaker_slot_w + (speaker_slot_count - 1) * speaker_slot_gap;

    for (i = [0 : speaker_slot_count - 1]) {
        x = -total_w / 2 + speaker_slot_w / 2 + i * (speaker_slot_w + speaker_slot_gap);

        translate([speaker_center_x + x, speaker_center_y, -1])
            rounded_slot_cutout(speaker_slot_w, speaker_slot_h, wall + 2);
    }
}

module speaker_rect_ring(outer_w, outer_h, inner_w, inner_h, h, r) {
    difference() {
        translate([
            speaker_center_x - outer_w / 2,
            speaker_center_y - outer_h / 2,
            wall
        ])
            rounded_cube([outer_w, outer_h, h], r);

        translate([
            speaker_center_x - inner_w / 2,
            speaker_center_y - inner_h / 2,
            wall - 0.1
        ])
            rounded_cube([inner_w, inner_h, h + 0.2], max(r - 0.7, 0.5));
    }
}

module speaker_acoustic_baffle() {
    speaker_rect_ring(
        speaker_baffle_outer_w,
        speaker_baffle_outer_h,
        speaker_baffle_inner_w,
        speaker_baffle_inner_h,
        speaker_baffle_h,
        speaker_baffle_corner_r
    );
}

module speaker_wire_relief_cutout() {
    // Notch through the right baffle wall, centered on the speaker's right side.
    // Only cuts the upper 50% of the baffle height.
    translate([
        speaker_center_x + speaker_baffle_inner_w / 2 - 0.1,
        speaker_center_y - speaker_wire_relief_w / 2,
        wall + speaker_baffle_h - speaker_wire_relief_h - 0.1
    ])
        cube([
            speaker_wire_relief_depth,
            speaker_wire_relief_w,
            speaker_wire_relief_h + 0.2
        ]);
}

module usb_cutout() {
    translate([
        usb_slot_x,
        case_h - wall - 1,
        usb_cutout_z
    ])
        cube([usb_slot_w, wall + 2, usb_cutout_h]);
}

module micro_sd_cutout() {
    translate([
        case_w - wall - 1,
        sd_slot_y,
        sd_slot_z
    ])
        cube([wall + 2, sd_slot_w, sd_slot_h]);
}

module usb_c_header_mount_points() {
    for (p = usb_c_hole_centers) {
        translate([p[0], p[1], 0]) children();
    }
}

module usb_c_header_floor_recess() {
    // 20mm x 7mm x 2.1mm flush recess from the outside speaker grille side.
    // This lets the USB-C PCB sit deeper in the outside bottom face.
    translate([
        usb_c_panel_x - usb_c_recess_fudge / 2,
        usb_c_panel_y - usb_c_recess_fudge / 2,
        -0.05
    ])
        cube([
            usb_c_panel_w + usb_c_recess_fudge,
            usb_c_panel_h + usb_c_recess_fudge,
            usb_c_panel_depth + 0.10
        ]);
}

module usb_c_header_body_cutout() {
    // Through cutout for the USB-C header body, on the same face as the speaker grille.
    translate([
        usb_c_panel_center_x - usb_c_header_cutout_w / 2,
        usb_c_panel_center_y - usb_c_header_cutout_h / 2,
        -1
    ])
        cube([
            usb_c_header_cutout_w,
            usb_c_header_cutout_h,
            wall + 2
        ]);
}

module usb_c_header_mount_holes() {
    // Blind pilot holes from the flush recess into the remaining base floor
    // and raised screw-support pads.
    usb_c_header_mount_points() {
        translate([0, 0, usb_c_panel_depth - 0.05])
            cylinder(
                h = wall + usb_c_support_rail_h - usb_c_panel_depth + 0.25,
                d = usb_c_mount_pilot_d
            );
    }
}

module usb_c_header_cutouts() {
    usb_c_header_floor_recess();
    usb_c_header_body_cutout();
    usb_c_header_mount_holes();
}

module usb_c_header_supports() {
    // Complete raised outline around the flush recess.
    // Top and bottom rails extend over the left/right rails so the four corners are populated.
    // Left and right screw-support pads extend from the outline to the USB-C body cutout.
    cutout_x0 = usb_c_panel_center_x - usb_c_header_cutout_w / 2;
    cutout_x1 = usb_c_panel_center_x + usb_c_header_cutout_w / 2;

    translate([
        usb_c_panel_x - usb_c_support_end_w,
        usb_c_panel_y - usb_c_support_rail_w,
        wall
    ])
        cube([
            usb_c_panel_w + usb_c_support_end_w * 2,
            usb_c_support_rail_w,
            usb_c_support_rail_h
        ]);

    translate([
        usb_c_panel_x - usb_c_support_end_w,
        usb_c_panel_y + usb_c_panel_h,
        wall
    ])
        cube([
            usb_c_panel_w + usb_c_support_end_w * 2,
            usb_c_support_rail_w,
            usb_c_support_rail_h
        ]);

    // Left screw support, filled from the outside outline to the cutout edge.
    translate([
        usb_c_panel_x - usb_c_support_end_w,
        usb_c_panel_y,
        wall
    ])
        cube([
            cutout_x0 - usb_c_panel_x + usb_c_support_end_w,
            usb_c_panel_h,
            usb_c_support_rail_h
        ]);

    // Right screw support, filled from the cutout edge to the outside outline.
    translate([
        cutout_x1,
        usb_c_panel_y,
        wall
    ])
        cube([
            usb_c_panel_x + usb_c_panel_w + usb_c_support_end_w - cutout_x1,
            usb_c_panel_h,
            usb_c_support_rail_h
        ]);
}

module base_part_number_text() {
    translate([base_part_number_x, base_part_number_y, wall])
        linear_extrude(height = base_part_number_h)
            text(
                base_part_number,
                size = base_part_number_size,
                halign = "center",
                valign = "center"
            );
}

module usb_c_polarity_marks() {
    translate([usb_c_polarity_plus_x, usb_c_polarity_mark_y, wall])
        linear_extrude(height = usb_c_polarity_mark_h)
            text(
                "+",
                size = usb_c_polarity_mark_size,
                halign = "center",
                valign = "center"
            );
}

module rubber_foot_recesses() {
    for (x = rubber_foot_x_positions) {
        for (z = rubber_foot_z_positions) {
            translate([x, -0.1, z])
                cube([
                    rubber_foot_size,
                    rubber_foot_recess_depth + 0.1,
                    rubber_foot_size
                ]);
        }
    }
}

function lid_mount_reach_x_raw(p) =
    (p[0] < case_w / 2)
        ? (p[0] + lid_mount_inner_extra - wall)
        : (case_w - wall - (p[0] - lid_mount_inner_extra));

function lid_mount_reach_y_raw(p) =
    (p[1] < case_h / 2)
        ? (p[1] + lid_mount_inner_extra - wall)
        : (case_h - wall - (p[1] - lid_mount_inner_extra));

function lid_mount_reach_x(p) = lid_mount_reach_x_raw(p) * lid_mount_reach_scale;
function lid_mount_reach_y(p) = lid_mount_reach_y_raw(p) * lid_mount_reach_scale;

module lid_corner_mount_block(p, reach_x, reach_y, zpos, block_h) {
    if (p[0] < case_w / 2 && p[1] < case_h / 2) {
        translate([wall - lid_mount_wall_overlap, wall - lid_mount_wall_overlap, zpos])
            cube([reach_x + lid_mount_wall_overlap, reach_y + lid_mount_wall_overlap, block_h]);
    }
    else if (p[0] >= case_w / 2 && p[1] < case_h / 2) {
        translate([case_w - wall - reach_x, wall - lid_mount_wall_overlap, zpos])
            cube([reach_x + lid_mount_wall_overlap, reach_y + lid_mount_wall_overlap, block_h]);
    }
    else if (p[0] < case_w / 2 && p[1] >= case_h / 2) {
        translate([wall - lid_mount_wall_overlap, case_h - wall - reach_y, zpos])
            cube([reach_x + lid_mount_wall_overlap, reach_y + lid_mount_wall_overlap, block_h]);
    }
    else {
        translate([case_w - wall - reach_x, case_h - wall - reach_y, zpos])
            cube([reach_x + lid_mount_wall_overlap, reach_y + lid_mount_wall_overlap, block_h]);
    }
}

module lid_corner_mount(p) {
    z0 = case_d - lid_mount_h;
    full_reach_x = lid_mount_reach_x(p);
    full_reach_y = lid_mount_reach_y(p);

    hull() {
        lid_corner_mount_block(
            p,
            lid_mount_base_reach,
            lid_mount_base_reach,
            z0,
            lid_mount_hull_slice_h
        );

        lid_corner_mount_block(
            p,
            full_reach_x,
            full_reach_y,
            z0 + lid_mount_grow_h - lid_mount_hull_slice_h,
            lid_mount_hull_slice_h
        );
    }

    lid_corner_mount_block(
        p,
        full_reach_x,
        full_reach_y,
        z0 + lid_mount_grow_h,
        lid_mount_top_h
    );
}

module lid_lip_corner_mount_relief(p, z0, zh) {
    reach_x = lid_mount_reach_x(p) + lid_mount_lip_clearance;
    reach_y = lid_mount_reach_y(p) + lid_mount_lip_clearance;

    if (p[0] < case_w / 2 && p[1] < case_h / 2) {
        translate([wall - 1.2, wall - 1.2, z0])
            cube([reach_x + 2.4, reach_y + 2.4, zh]);
    }
    else if (p[0] >= case_w / 2 && p[1] < case_h / 2) {
        translate([case_w - wall - reach_x - 1.2, wall - 1.2, z0])
            cube([reach_x + 2.4, reach_y + 2.4, zh]);
    }
    else if (p[0] < case_w / 2 && p[1] >= case_h / 2) {
        translate([wall - 1.2, case_h - wall - reach_y - 1.2, z0])
            cube([reach_x + 2.4, reach_y + 2.4, zh]);
    }
    else {
        translate([case_w - wall - reach_x - 1.2, case_h - wall - reach_y - 1.2, z0])
            cube([reach_x + 2.4, reach_y + 2.4, zh]);
    }
}

module lid_lip_mount_relief() {
    for (p = lid_points())
        lid_lip_corner_mount_relief(
            p,
            -lip_depth - 0.6,
            lip_depth + 1.2
        );
}

module case_shell() {
    difference() {
        rounded_cube([case_w, case_h, case_d], corner_r);

        translate([wall, wall, wall])
            rounded_cube(
                [case_w - wall * 2, case_h - wall * 2, case_d + 2],
                max(corner_r - wall, 0.5)
            );

        // USB cutout intentionally removed.
        micro_sd_cutout();
        rubber_foot_recesses();
        speaker_grill_holes();
    }
}

module base_cutouts() {
    // Blind lid support pilot holes.
    // These do not cut through the lower wall or the extended support end.
    for (p = lid_points()) {
        translate([p[0], p[1], case_d - lid_mount_blind_pilot_depth])
            cylinder(
                d = lid_mount_pilot_d,
                h = lid_mount_blind_pilot_depth + 0.2
            );

        translate([p[0], p[1], case_d - lid_mount_pilot_entry_h])
            cylinder(
                d = lid_mount_pilot_entry_d,
                h = lid_mount_pilot_entry_h + 0.3
            );
    }

    usb_c_header_cutouts();

    pi_mount_holes() {
        translate([0, 0, wall - 1])
            cylinder(h = pi_standoff_h + 2, d = pi_standoff_pilot_d);
    }

    amp_mount_holes() {
        translate([0, 0, wall - 1])
            cylinder(h = amp_standoff_h + 2, d = amp_standoff_pilot_d);
    }

    speaker_mount_points() {
        translate([0, 0, wall - 1])
            cylinder(h = speaker_mount_h + 2, d = speaker_mount_pilot_d);
    }

    speaker_pocket_keepout();
    speaker_wire_relief_cutout();
}

module base() {
    difference() {
        union() {
            case_shell();

            for (p = lid_points())
                lid_corner_mount(p);

            speaker_acoustic_baffle();
            speaker_mount_supports();
            pi_standoff_gussets();
            amp_standoff_gussets();
            usb_c_header_supports();
            base_part_number_text();
            usb_c_polarity_marks();

            pi_mount_holes() {
                translate([0, 0, wall])
                    cylinder(h = pi_standoff_h, d = pi_standoff_od);
            }

            amp_mount_holes() {
                translate([0, 0, wall])
                    cylinder(h = amp_standoff_h, d = amp_standoff_od);
            }

            // Speaker mount pilots are cut into the flat wall-to-baffle tabs above.
            // No round speaker boss cylinders are added here.
        }

        base_cutouts();
    }
}

module lid() {
    difference() {
        union() {
            rounded_cube([case_w, case_h, lid_t], corner_r);
            lid_lip_ring();
            oled_solder_padding_pads();
        }

        lid_lip_mount_relief();

        oled_screen_cutout();
        oled_m3_lid_holes();

        for (p = lid_points()) {
            translate([p[0], p[1], -1])
                cylinder(h = lid_t + 2, d = lid_screw_d);

            translate([p[0], p[1], lid_t - lid_screw_head_recess_h])
                cylinder(
                    h = lid_screw_head_recess_h + 0.3,
                    d1 = lid_screw_d,
                    d2 = lid_screw_head_d
                );
        }
    }
}

module oled_preview() {
    lid_z = case_d + assembly_lid_lift;

    color("green", 0.35)
        translate([oled_x, oled_y, lid_z - oled_solder_joint_padding - oled_pcb_t])
            cube([oled_pcb_w, oled_pcb_h, oled_pcb_t]);

    color("black", 0.65)
        translate([oled_screen_x, oled_screen_y, lid_z + lid_t - 0.03])
            cube([oled_screen_w, oled_screen_h, 0.06]);

    color("gray", 0.35)
        translate([oled_view_x, oled_view_y, lid_z + lid_t + 0.02])
            cube([oled_view_w, oled_view_h, 0.04]);

    oled_holes() {
        color("silver", 0.45)
            translate([0, 0, lid_z + lid_t + 0.05])
                cylinder(h = 0.6, d = oled_m3_head_preview_d);
    }

    oled_holes() {
        color("silver", 0.35)
            translate([0, 0, lid_z - oled_solder_joint_padding - oled_pcb_t - 0.45])
                cylinder(h = 0.45, d = oled_m3_washer_preview_d);

        color("silver", 0.45)
            translate([0, 0, lid_z - oled_solder_joint_padding - oled_pcb_t - 1.35])
                rotate([0, 0, 30])
                    cylinder(h = 0.9, d = oled_m3_nut_preview_d, $fn = 6);
    }
}

module pi_zero_preview() {
    color("green", 0.35)
        translate([pi_x, pi_y, wall + pi_standoff_h + 0.3])
            rounded_cube([pi_w, pi_h, 1.6], pi_corner_r);

    for (x = usb_centers_x) {
        color("silver", 0.55)
            translate([x - 4.5, pi_y + pi_h - 4, wall + pi_standoff_h + 2])
                cube([9, 8, 7]);
    }

    color("silver", 0.25)
        translate([micro_hdmi_center_x - 5, pi_y + pi_h - 4, wall + pi_standoff_h + 2])
            cube([10, 8, 5]);
}

module amp_preview() {
    color("blue", 0.35)
        translate([amp_x, amp_y, wall + amp_standoff_h + 0.3])
            rounded_cube([amp_w, amp_h, 1.6], amp_corner_r);

    color("gray", 0.35)
        amp_mount_holes() {
            translate([0, 0, wall + amp_standoff_h + 1.9])
                cylinder(h = 0.8, d = 4.5);
        }
}

module usb_c_header_preview() {
    color("green", 0.35)
        translate([
            usb_c_panel_x,
            usb_c_panel_y,
            usb_c_panel_z
        ])
            cube([usb_c_panel_w, usb_c_panel_h, usb_c_panel_depth]);

    color("silver", 0.45)
        translate([
            usb_c_panel_center_x - usb_c_connector_preview_w / 2,
            usb_c_panel_center_y - usb_c_connector_preview_d / 2,
            usb_c_panel_depth
        ])
            cube([
                usb_c_connector_preview_w,
                usb_c_connector_preview_d,
                usb_c_connector_preview_h
            ]);

    color("gray", 0.35)
        usb_c_header_mount_points() {
            translate([0, 0, wall + 0.05])
                cylinder(h = 0.6, d = 4.5);
        }
}

module speaker_preview() {
    color("black", 0.35)
        translate([
            speaker_center_x - speaker_w / 2,
            speaker_center_y - speaker_h / 2,
            wall + speaker_mount_h + 0.2
        ])
            cube([speaker_w, speaker_h, 4]);

    color("gray", 0.35)
        speaker_mount_points() {
            translate([0, 0, wall + speaker_mount_h + 4.3])
                cylinder(h = 0.8, d = 4.5);
        }
}

if (part == "base") {
    base();
}

if (part == "lid") {
    lid();
}

if (part == "assembly") {
    base();

    translate([0, 0, case_d + assembly_lid_lift])
        lid();

    oled_preview();
    pi_zero_preview();
    amp_preview();
    usb_c_header_preview();
    speaker_preview();
}
