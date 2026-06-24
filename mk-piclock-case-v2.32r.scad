// mk-piclock case V2.3R
// Base geometry retained from mk-piclock-case-v2.51.scad
// Lid geometry retained from mk-piclock-case-v2.52F-marked.scad
//
// Released: 2026-06-24
//
// Complete single-file OpenSCAD source. No include or use statements.
//
// Current touch-sensor design:
// 1. Normal case wall remains 3mm thick.
// 2. Touch target is a smooth 14mm circular depression.
// 3. Depression leaves exactly 1mm over the sensing electrode.
// 4. A raised 2mm right-hand crescent surrounds the circle.
// 5. Two diagonal mounts are retained:
//    - lower-left external M2.5 clearance screw
//    - upper-right internal blind pilot
// 6. Sensor remains lowered 2mm from its original position.
// 7. Upper-right touch pilot leaves exactly 1mm of exterior material.
// 8. Original OLED lid clearance holes and half-moon pads are restored.
// 9. No temperature or humidity sensor mount is included.
//
// Units: mm

$fn = 64;

// Select: "base", "lid", or "assembly".
part = "base";

fit_fudge = 0.3;

// -----------------------------------------------------------------------------
// Case
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// OLED module
// -----------------------------------------------------------------------------

oled_pcb_w = 100.5;
oled_pcb_h = 33.5;

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

oled_m3_clearance_d = 3.0 + fit_fudge;
oled_m3_head_preview_d = 5.8;
oled_m3_washer_preview_d = 7.0;
oled_m3_nut_preview_d = 6.4;

oled_solder_joint_padding = 3.0;
oled_solder_padding_d = 6.4;

// -----------------------------------------------------------------------------
// Raspberry Pi Zero and amplifier
// -----------------------------------------------------------------------------

pi_w = 65;
pi_h = 30;
pi_corner_r = 3;

pi_x = case_w - wall - 2 - pi_w;

board_gap = 2;
pi_shift_y = -1.5;

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

pi_standoff_h = 6;
pi_standoff_od = 5.8;
pi_standoff_pilot_d = 1.8;

pi_gusset_h = 4.5;
pi_gusset_w = 1.6;
pi_gusset_len = 6;

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

// -----------------------------------------------------------------------------
// Speaker
// -----------------------------------------------------------------------------

speaker_w = 31;
speaker_h = 28;

speaker_shift_x = -3;
speaker_shift_y = 0;
speaker_center_x = wall + (pi_x - wall) / 2 + speaker_shift_x;
speaker_center_y = case_h / 2 + speaker_shift_y;

speaker_mount_nominal_h = 7;
speaker_baffle_h = speaker_mount_nominal_h;
speaker_mount_h = speaker_baffle_h;
speaker_mount_w = pi_standoff_od;
speaker_mount_pilot_d = pi_standoff_pilot_d;
speaker_mount_span = 38.0;

speaker_mount_center_to_wall =
    (case_h - 2 * wall - speaker_mount_span) / 2;
speaker_mount_wall_clearance =
    speaker_mount_center_to_wall - speaker_mount_w / 2;

speaker_mount_support_w = speaker_mount_w;
speaker_mount_support_h = speaker_baffle_h;
speaker_support_wall_overlap = 0.8;

speaker_grill_d = 28;
speaker_slot_w = 2.2;
speaker_slot_h = 22.0;
speaker_slot_gap = 1.25;
speaker_slot_count = 7;

speaker_baffle_wall = 1.4;
speaker_pocket_clearance = 1.0;
speaker_pocket_w = speaker_w + speaker_pocket_clearance;
speaker_pocket_h = speaker_h + speaker_pocket_clearance;
speaker_baffle_outer_w = speaker_pocket_w + speaker_baffle_wall * 2;
speaker_baffle_outer_h = speaker_pocket_h + speaker_baffle_wall * 2;
speaker_baffle_inner_w = speaker_pocket_w;
speaker_baffle_inner_h = speaker_pocket_h;
speaker_baffle_corner_r = 2.0;

speaker_wire_relief_w = 4.0;
speaker_wire_relief_depth = speaker_baffle_wall + 0.8;
speaker_wire_relief_h = speaker_baffle_h * 0.5;

// -----------------------------------------------------------------------------
// Pi ports
// -----------------------------------------------------------------------------

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

sd_slot_w_original = 18;
sd_slot_trim_right = 4;
sd_slot_trim_foot_side = 1;
sd_slot_trim_hdmi_side = 1;
sd_slot_w =
    sd_slot_w_original
    - sd_slot_trim_right
    - sd_slot_trim_foot_side
    - sd_slot_trim_hdmi_side;

sd_slot_h = 6;
sd_slot_z = wall + pi_standoff_h;
sd_slot_y =
    pi_y + pi_h / 2
    - sd_slot_w_original / 2
    + sd_slot_trim_foot_side;

micro_hdmi_center_x = pi_x + (pi_w - 12.4);

// -----------------------------------------------------------------------------
// USB-C power insert
// -----------------------------------------------------------------------------

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

usb_c_panel_x =
    case_w - wall - usb_c_clearance_from_walls - usb_c_panel_w;
usb_c_panel_y = wall + usb_c_clearance_from_walls;
usb_c_panel_center_x = usb_c_panel_x + usb_c_panel_w / 2;
usb_c_panel_center_y = usb_c_panel_y + usb_c_panel_h / 2;
usb_c_panel_z = 0;

usb_c_hole_centers = [
    [usb_c_panel_center_x - usb_c_hole_span / 2, usb_c_panel_center_y],
    [usb_c_panel_center_x + usb_c_hole_span / 2, usb_c_panel_center_y]
];

usb_c_support_rail_w = 1.4;
usb_c_support_rail_h = 5.0;
usb_c_support_end_w = 1.4;

usb_c_connector_preview_w = usb_c_header_cutout_w;
usb_c_connector_preview_d = usb_c_header_cutout_h;
usb_c_connector_preview_h = 6.0;

// -----------------------------------------------------------------------------
// Markings and rubber feet
// -----------------------------------------------------------------------------

base_part_number = "mK-piclock v2.32r";
base_part_number_size = 4.0;
base_part_number_h = 0.45;
base_part_number_x = pi_x + pi_w / 2;
base_part_number_y = pi_y + pi_h / 2;

// Raised on the lid underside, below the OLED PCB footprint.
// Mirrored so it reads correctly when viewing the removed lid from inside.
lid_part_number = "MK-PiClock-V2.3R";
lid_part_number_size = 3.4;
lid_part_number_h = 0.55;
lid_text_overlap = 0.05;
lid_part_number_x = case_w / 2;
lid_part_number_y = 11.0;

// Lid orientation marks, raised on the underside and clear of the OLED PCB.
lid_orientation_text_size = 3.6;
lid_orientation_text_h = 0.55;
lid_top_text_x = case_w / 2;
lid_top_clear_edge_y = case_h - (wall + lip_clearance + lip_wall);
lid_top_text_y = (oled_y + oled_pcb_h + lid_top_clear_edge_y) / 2;

usb_c_polarity_mark_size = 4.0;
usb_c_polarity_mark_h = 0.45;
usb_c_polarity_mark_y = usb_c_panel_y - 4.0;
usb_c_polarity_plus_x =
    usb_c_panel_center_x - usb_c_hole_span / 2;

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

// -----------------------------------------------------------------------------
// Touch sensor
// -----------------------------------------------------------------------------

touch_pcb_size = 24.0;
touch_pcb_t = 1.6;
touch_hole_span = 20.0;
touch_m25_clearance_d = 2.9;

touch_m25_head_d = 5.5;
touch_m25_head_h = 1.6;
touch_m25_head_seat_depth = 0.5;

// Lowered 2mm from the original Z=31mm position.
touch_center_z = 29.0;

touch_pad_d = 14.0;
touch_component_strip_w = touch_pcb_size - touch_pad_d;
touch_pcb_offset_x = touch_component_strip_w / 2;

touch_center_x = 87.0;
touch_pad_center_x = touch_center_x + touch_pcb_offset_x;
touch_pad_center_z = touch_center_z;

touch_solder_x_size = 3.0;
touch_solder_z_size = 8.0;
touch_solder_center_from_left_edge = 2.7;
touch_solder_clearance = 1.7;
touch_solder_center_x =
    touch_center_x
    - touch_pcb_size / 2
    + touch_solder_center_from_left_edge;
touch_solder_center_z = touch_center_z;

// The PCB sits against the normal 3mm inner wall.
touch_wall_thickness = wall;
touch_panel_inner_y = case_h - wall;
touch_pcb_outer_face_y = touch_panel_inner_y;
touch_solder_floor_thickness = wall - touch_solder_clearance;

touch_sensing_skin_thickness = 1.0;
touch_circle_recess_d = touch_pad_d;
touch_circle_recess_depth = wall - touch_sensing_skin_thickness;

touch_button_d = 14.0;
touch_chamfer_width = 0.4;
touch_chamfer_depth = 0.4;
touch_chamfer_outer_d = touch_button_d + 2 * touch_chamfer_width;
touch_circle_fn = 96;

// Raised right-hand crescent.
touch_crescent_raise = 2.0;

// Retained Pi-style pilot diameter.
touch_right_pilot_d = pi_standoff_pilot_d;

// Upper-right screw enters from inside and stops 1mm below the raised exterior.
touch_right_outer_skin_thickness = 1.0;
touch_right_local_thickness = wall + touch_crescent_raise;
touch_right_pilot_depth =
    touch_right_local_thickness - touch_right_outer_skin_thickness;

touch_tool_access_d = 7.75;
touch_tool_access_z_shift = -1.0;
touch_crescent_center_x = touch_pad_center_x;
touch_crescent_center_z = touch_pad_center_z;
touch_crescent_gap = 0.8;
touch_crescent_inner_r_base =
    touch_chamfer_outer_d / 2 + touch_crescent_gap;

touch_right_screw_radius = sqrt(
    pow(
        touch_center_x + touch_hole_span / 2 - touch_pad_center_x,
        2
    )
    + pow(touch_hole_span / 2, 2)
);

touch_crescent_outer_margin = 2.8;
touch_crescent_outer_r_base =
    touch_right_screw_radius + touch_crescent_outer_margin;
touch_crescent_inner_r_face =
    touch_crescent_inner_r_base + touch_crescent_raise;
touch_crescent_outer_r_face =
    touch_crescent_outer_r_base - touch_crescent_raise;
touch_crescent_fn = 96;
touch_crescent_layer_h = 0.12;

// -----------------------------------------------------------------------------
// Lid fasteners and supports
// -----------------------------------------------------------------------------

lid_screw_margin = 7;
lid_screw_d = 3.0;
lid_screw_head_d = 6.8;
lid_screw_head_recess_h = 1.8;

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
lid_mount_reach_scale = 1.30;
lid_mount_lip_clearance = 1.2;
lid_mount_hull_slice_h = 0.25;
lid_mount_blind_pilot_depth = 10.0;

function lid_points() = [
    [lid_screw_margin, lid_screw_margin],
    [case_w - lid_screw_margin, lid_screw_margin],
    [lid_screw_margin, case_h - lid_screw_margin],
    [case_w - lid_screw_margin, case_h - lid_screw_margin]
];

function lid_mount_reach_x(p) =
    (
        p[0] < case_w / 2
        ? p[0] - wall + lid_mount_inner_extra
        : case_w - wall - p[0] + lid_mount_inner_extra
    ) * lid_mount_reach_scale;

function lid_mount_reach_y(p) =
    (
        p[1] < case_h / 2
        ? p[1] - wall + lid_mount_inner_extra
        : case_h - wall - p[1] + lid_mount_inner_extra
    ) * lid_mount_reach_scale;

// -----------------------------------------------------------------------------
// General geometry helpers
// -----------------------------------------------------------------------------

module rounded_cube(size, r) {
    hull() {
        translate([r, r, 0])
            cylinder(h = size[2], r = r);
        translate([size[0] - r, r, 0])
            cylinder(h = size[2], r = r);
        translate([r, size[1] - r, 0])
            cylinder(h = size[2], r = r);
        translate([size[0] - r, size[1] - r, 0])
            cylinder(h = size[2], r = r);
    }
}

module oled_holes() {
    translate([
        oled_x + oled_hole_x_offset,
        oled_y + oled_hole_y_offset,
        0
    ]) children();

    translate([
        oled_x + oled_pcb_w - oled_hole_x_offset,
        oled_y + oled_hole_y_offset,
        0
    ]) children();

    translate([
        oled_x + oled_hole_x_offset,
        oled_y + oled_pcb_h - oled_hole_y_offset,
        0
    ]) children();

    translate([
        oled_x + oled_pcb_w - oled_hole_x_offset,
        oled_y + oled_pcb_h - oled_hole_y_offset,
        0
    ]) children();
}

module pi_mount_holes() {
    for (p = pi_holes)
        translate([pi_x + p[0], pi_y + p[1], 0])
            children();
}

module amp_mount_holes() {
    for (p = amp_holes)
        translate([amp_x + p[0], amp_y + p[1], 0])
            children();
}

// -----------------------------------------------------------------------------
// OLED lid geometry
// -----------------------------------------------------------------------------

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
        oled_half_moon_padding_pad(
            atan2(ly - oled_screen_cy, ll_x - oled_screen_cx)
        );

    translate([lr_x, ly, 0])
        oled_half_moon_padding_pad(
            atan2(ly - oled_screen_cy, lr_x - oled_screen_cx)
        );

    translate([ll_x, uy, 0])
        oled_half_moon_padding_pad(
            atan2(uy - oled_screen_cy, ll_x - oled_screen_cx)
        );

    translate([lr_x, uy, 0])
        oled_half_moon_padding_pad(
            atan2(uy - oled_screen_cy, lr_x - oled_screen_cx)
        );
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

// -----------------------------------------------------------------------------
// Pi and amp supports
// -----------------------------------------------------------------------------

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
    pi_standoff_gusset_at(pi_x + 3.5, pi_y + 3.5, 45);
    pi_standoff_gusset_at(pi_x + 61.5, pi_y + 3.5, 135);
    pi_standoff_gusset_at(pi_x + 3.5, pi_y + 26.5, 315);
    pi_standoff_gusset_at(pi_x + 61.5, pi_y + 26.5, 225);
}

module amp_standoff_gussets() {
    pi_standoff_gusset_at(amp_hole_x, amp_lower_hole_y, 0);
    pi_standoff_gusset_at(amp_hole_x, amp_upper_hole_y, 0);
}

module pi_upper_right_anchor() {
    pi_anchor_d = 2.4;
    pi_anchor_h = 1.8;
    pi_anchor_tip_h = 0.4;
    straight_h = pi_anchor_h - pi_anchor_tip_h;
    anchor_x = pi_x + pi_holes[3][0];
    anchor_y = pi_y + pi_holes[3][1];

    translate([anchor_x, anchor_y, wall + pi_standoff_h]) {
        cylinder(h = straight_h, d = pi_anchor_d);

        translate([0, 0, straight_h])
            cylinder(
                h = pi_anchor_tip_h,
                d1 = pi_anchor_d,
                d2 = pi_anchor_d - 0.6
            );
    }
}

module pi_three_pilot_holes() {
    for (i = [0 : 2]) {
        translate([
            pi_x + pi_holes[i][0],
            pi_y + pi_holes[i][1],
            wall - 1
        ])
            cylinder(
                h = pi_standoff_h + 2,
                d = pi_standoff_pilot_d
            );
    }
}

// -----------------------------------------------------------------------------
// Speaker geometry
// -----------------------------------------------------------------------------

module speaker_mount_points() {
    translate([
        speaker_center_x,
        speaker_center_y - speaker_mount_span / 2,
        0
    ]) children();

    translate([
        speaker_center_x,
        speaker_center_y + speaker_mount_span / 2,
        0
    ]) children();
}

module speaker_support_arm_to_bottom_wall(x1, y1, x2) {
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

module speaker_mount_supports() {
    speaker_support_arm_to_bottom_wall(
        speaker_center_x,
        speaker_center_y - speaker_mount_span / 2,
        speaker_center_x
    );

    speaker_support_arm_to_top_wall(
        speaker_center_x,
        speaker_center_y + speaker_mount_span / 2,
        speaker_center_x
    );
}

module speaker_pocket_keepout() {
    translate([
        speaker_center_x - speaker_pocket_w / 2,
        speaker_center_y - speaker_pocket_h / 2,
        wall - 0.05
    ])
        rounded_cube(
            [
                speaker_pocket_w,
                speaker_pocket_h,
                speaker_mount_h + 1.0
            ],
            max(speaker_baffle_corner_r - 0.7, 0.5)
        );
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
    total_w =
        speaker_slot_count * speaker_slot_w
        + (speaker_slot_count - 1) * speaker_slot_gap;

    for (i = [0 : speaker_slot_count - 1]) {
        x =
            -total_w / 2
            + speaker_slot_w / 2
            + i * (speaker_slot_w + speaker_slot_gap);

        translate([speaker_center_x + x, speaker_center_y, -1])
            rounded_slot_cutout(
                speaker_slot_w,
                speaker_slot_h,
                wall + 2
            );
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
            rounded_cube(
                [inner_w, inner_h, h + 0.2],
                max(r - 0.7, 0.5)
            );
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

// -----------------------------------------------------------------------------
// Port and USB-C geometry
// -----------------------------------------------------------------------------

module micro_sd_cutout() {
    translate([
        case_w - wall - 1,
        sd_slot_y,
        sd_slot_z
    ])
        cube([wall + 2, sd_slot_w, sd_slot_h]);
}

module usb_c_header_mount_points() {
    for (p = usb_c_hole_centers)
        translate([p[0], p[1], 0])
            children();
}

module usb_c_header_floor_recess() {
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
    usb_c_header_mount_points() {
        translate([0, 0, usb_c_panel_depth - 0.05])
            cylinder(
                h =
                    wall
                    + usb_c_support_rail_h
                    - usb_c_panel_depth
                    + 0.25,
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

    translate([
        cutout_x1,
        usb_c_panel_y,
        wall
    ])
        cube([
            usb_c_panel_x
                + usb_c_panel_w
                + usb_c_support_end_w
                - cutout_x1,
            usb_c_panel_h,
            usb_c_support_rail_h
        ]);
}

// -----------------------------------------------------------------------------
// Markings and foot recesses
// -----------------------------------------------------------------------------

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

module lid_part_number_text() {
    translate([
        lid_part_number_x,
        lid_part_number_y,
        -lid_part_number_h + lid_text_overlap
    ])
        linear_extrude(height = lid_part_number_h)
            mirror([1, 0, 0])
                text(
                    lid_part_number,
                    size = lid_part_number_size,
                    halign = "center",
                    valign = "center"
                );
}

module lid_orientation_text() {
    translate([
        lid_top_text_x,
        lid_top_text_y,
        -lid_orientation_text_h + lid_text_overlap
    ])
        linear_extrude(height = lid_orientation_text_h)
            mirror([1, 0, 0])
                text(
                    "TOP",
                    size = lid_orientation_text_size,
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

// -----------------------------------------------------------------------------
// Current touch-sensor geometry
// -----------------------------------------------------------------------------

module touch_sensor_lower_left_point(y_pos = case_h - wall - 1) {
    translate([
        touch_center_x - touch_hole_span / 2,
        y_pos,
        touch_center_z - touch_hole_span / 2
    ]) children();
}

module touch_sensor_upper_right_point(y_pos = case_h - wall - 1) {
    translate([
        touch_center_x + touch_hole_span / 2,
        y_pos,
        touch_center_z + touch_hole_span / 2
    ]) children();
}

module touch_sensor_mount_holes() {
    // Lower-left through-wall M2.5 clearance hole.
    touch_sensor_lower_left_point(case_h - wall - 1) {
        rotate([-90, 0, 0])
            cylinder(
                h = wall + 2,
                d = touch_m25_clearance_d
            );
    }

    // Upper-right blind pilot through the wall and raised crescent.
    touch_sensor_upper_right_point(touch_panel_inner_y - 0.05) {
        rotate([-90, 0, 0])
            cylinder(
                h = touch_right_pilot_depth + 0.05,
                d = touch_right_pilot_d
            );
    }
}

module touch_sensor_head_seat() {
    touch_sensor_lower_left_point() {
        translate([0, wall + 1.05, 0])
            rotate([90, 0, 0])
                cylinder(
                    h = touch_m25_head_seat_depth + 0.10,
                    d = touch_m25_head_d
                );
    }
}

module touch_sensor_tool_access() {
    // Only the retained upper-right internal screw needs tool access.
    translate([
        touch_center_x + touch_hole_span / 2,
        -1,
        touch_center_z
            + touch_hole_span / 2
            + touch_tool_access_z_shift
    ])
        rotate([-90, 0, 0])
            cylinder(
                h = wall + 2,
                d = touch_tool_access_d
            );
}

module touch_sensor_solder_pocket() {
    translate([
        touch_solder_center_x - touch_solder_x_size / 2,
        touch_panel_inner_y - 0.05,
        touch_solder_center_z - touch_solder_z_size / 2
    ])
        cube([
            touch_solder_x_size,
            touch_solder_clearance + 0.05,
            touch_solder_z_size
        ]);
}

module touch_sensor_smooth_recess() {
    translate([
        touch_pad_center_x,
        case_h + 0.05,
        touch_pad_center_z
    ])
        rotate([90, 0, 0]) {
            cylinder(
                h = touch_circle_recess_depth + 0.05,
                d = touch_button_d,
                $fn = touch_circle_fn
            );

            cylinder(
                h = touch_chamfer_depth + 0.05,
                d1 = touch_chamfer_outer_d,
                d2 = touch_button_d,
                $fn = touch_circle_fn
            );
        }
}

module touch_crescent_outer_frustum() {
    hull() {
        translate([
            touch_crescent_center_x,
            case_h - 0.06,
            touch_crescent_center_z
        ])
            rotate([-90, 0, 0])
                cylinder(
                    h = touch_crescent_layer_h,
                    r = touch_crescent_outer_r_base,
                    $fn = touch_crescent_fn
                );

        translate([
            touch_crescent_center_x,
            case_h
                + touch_crescent_raise
                - touch_crescent_layer_h,
            touch_crescent_center_z
        ])
            rotate([-90, 0, 0])
                cylinder(
                    h = touch_crescent_layer_h + 0.06,
                    r = touch_crescent_outer_r_face,
                    $fn = touch_crescent_fn
                );
    }
}

module touch_crescent_inner_frustum() {
    hull() {
        translate([
            touch_crescent_center_x,
            case_h - 0.20,
            touch_crescent_center_z
        ])
            rotate([-90, 0, 0])
                cylinder(
                    h = touch_crescent_layer_h + 0.30,
                    r = touch_crescent_inner_r_base,
                    $fn = touch_crescent_fn
                );

        translate([
            touch_crescent_center_x,
            case_h + touch_crescent_raise - 0.20,
            touch_crescent_center_z
        ])
            rotate([-90, 0, 0])
                cylinder(
                    h = touch_crescent_layer_h + 0.40,
                    r = touch_crescent_inner_r_face,
                    $fn = touch_crescent_fn
                );
    }
}

module touch_sensor_raised_crescent() {
    intersection() {
        difference() {
            touch_crescent_outer_frustum();
            touch_crescent_inner_frustum();
        }

        // Keep only the right-hand half: ")".
        translate([
            touch_crescent_center_x,
            case_h - 0.30,
            touch_crescent_center_z
                - touch_crescent_outer_r_base
                - 1
        ])
            cube([
                touch_crescent_outer_r_base + 2,
                touch_crescent_raise + 0.60,
                touch_crescent_outer_r_base * 2 + 2
            ]);
    }
}

// -----------------------------------------------------------------------------
// Case shell and lid supports
// -----------------------------------------------------------------------------

module case_shell() {
    difference() {
        rounded_cube([case_w, case_h, case_d], corner_r);

        // Open interior and open top.
        translate([wall, wall, wall])
            rounded_cube(
                [
                    case_w - 2 * wall,
                    case_h - 2 * wall,
                    case_d + 1
                ],
                max(corner_r - wall, 0.5)
            );

        micro_sd_cutout();
        speaker_grill_holes();
        rubber_foot_recesses();
    }
}

module lid_corner_mount_block(p, reach_x, reach_y, zpos, block_h) {
    if (p[0] < case_w / 2 && p[1] < case_h / 2) {
        translate([
            wall - lid_mount_wall_overlap,
            wall - lid_mount_wall_overlap,
            zpos
        ])
            cube([
                reach_x + lid_mount_wall_overlap,
                reach_y + lid_mount_wall_overlap,
                block_h
            ]);
    }
    else if (p[0] >= case_w / 2 && p[1] < case_h / 2) {
        translate([
            case_w - wall - reach_x,
            wall - lid_mount_wall_overlap,
            zpos
        ])
            cube([
                reach_x + lid_mount_wall_overlap,
                reach_y + lid_mount_wall_overlap,
                block_h
            ]);
    }
    else if (p[0] < case_w / 2 && p[1] >= case_h / 2) {
        translate([
            wall - lid_mount_wall_overlap,
            case_h - wall - reach_y,
            zpos
        ])
            cube([
                reach_x + lid_mount_wall_overlap,
                reach_y + lid_mount_wall_overlap,
                block_h
            ]);
    }
    else {
        translate([
            case_w - wall - reach_x,
            case_h - wall - reach_y,
            zpos
        ])
            cube([
                reach_x + lid_mount_wall_overlap,
                reach_y + lid_mount_wall_overlap,
                block_h
            ]);
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

module lid() {
    difference() {
        union() {
            rounded_cube([case_w, case_h, lid_t], corner_r);
            lid_lip_ring();
            oled_solder_padding_pads();
            lid_part_number_text();
            lid_orientation_text();
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

// -----------------------------------------------------------------------------
// Base
// -----------------------------------------------------------------------------

module base_cutouts() {
    for (p = lid_points()) {
        translate([
            p[0],
            p[1],
            case_d - lid_mount_blind_pilot_depth
        ])
            cylinder(
                d = lid_mount_pilot_d,
                h = lid_mount_blind_pilot_depth + 0.2
            );

        translate([
            p[0],
            p[1],
            case_d - lid_mount_pilot_entry_h
        ])
            cylinder(
                d = lid_mount_pilot_entry_d,
                h = lid_mount_pilot_entry_h + 0.3
            );
    }

    usb_c_header_cutouts();

    touch_sensor_mount_holes();
    touch_sensor_head_seat();
    touch_sensor_tool_access();
    touch_sensor_solder_pocket();
    touch_sensor_smooth_recess();

    pi_three_pilot_holes();

    amp_mount_holes() {
        translate([0, 0, wall - 1])
            cylinder(
                h = amp_standoff_h + 2,
                d = amp_standoff_pilot_d
            );
    }

    speaker_mount_points() {
        translate([0, 0, wall - 1])
            cylinder(
                h = speaker_mount_h + 2,
                d = speaker_mount_pilot_d
            );
    }

    speaker_pocket_keepout();
    speaker_wire_relief_cutout();
}

module base() {
    difference() {
        union() {
            case_shell();
            touch_sensor_raised_crescent();

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
                    cylinder(
                        h = pi_standoff_h,
                        d = pi_standoff_od
                    );
            }

            pi_upper_right_anchor();

            amp_mount_holes() {
                translate([0, 0, wall])
                    cylinder(
                        h = amp_standoff_h,
                        d = amp_standoff_od
                    );
            }
        }

        base_cutouts();
    }
}

// -----------------------------------------------------------------------------
// Assembly previews
// -----------------------------------------------------------------------------

module oled_preview() {
    lid_z = case_d + assembly_lid_lift;

    color("green", 0.35)
        translate([
            oled_x,
            oled_y,
            lid_z - oled_solder_joint_padding - oled_pcb_t
        ])
            cube([oled_pcb_w, oled_pcb_h, oled_pcb_t]);

    color("black", 0.65)
        translate([
            oled_screen_x,
            oled_screen_y,
            lid_z + lid_t - 0.03
        ])
            cube([oled_screen_w, oled_screen_h, 0.06]);

    color("gray", 0.35)
        translate([
            oled_view_x,
            oled_view_y,
            lid_z + lid_t + 0.02
        ])
            cube([oled_view_w, oled_view_h, 0.04]);

    oled_holes() {
        color("silver", 0.45)
            translate([0, 0, lid_z + lid_t + 0.05])
                cylinder(h = 0.6, d = oled_m3_head_preview_d);
    }
}

module pi_zero_preview() {
    color("green", 0.35)
        translate([pi_x, pi_y, wall + pi_standoff_h + 0.3])
            rounded_cube([pi_w, pi_h, 1.6], pi_corner_r);

    for (x = usb_centers_x) {
        color("silver", 0.55)
            translate([
                x - 4.5,
                pi_y + pi_h - 4,
                wall + pi_standoff_h + 2
            ])
                cube([9, 8, 7]);
    }

    color("silver", 0.25)
        translate([
            micro_hdmi_center_x - 5,
            pi_y + pi_h - 4,
            wall + pi_standoff_h + 2
        ])
            cube([10, 8, 5]);
}

module amp_preview() {
    color("blue", 0.35)
        translate([amp_x, amp_y, wall + amp_standoff_h + 0.3])
            rounded_cube([amp_w, amp_h, 1.6], amp_corner_r);
}

module usb_c_header_preview() {
    color("green", 0.35)
        translate([usb_c_panel_x, usb_c_panel_y, usb_c_panel_z])
            cube([
                usb_c_panel_w,
                usb_c_panel_h,
                usb_c_panel_depth
            ]);

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
}

module touch_sensor_preview() {
    pcb_outer_face_y = touch_pcb_outer_face_y;
    pcb_inner_face_y = pcb_outer_face_y - touch_pcb_t;

    color("green", 0.35)
        translate([
            touch_center_x - touch_pcb_size / 2,
            pcb_inner_face_y,
            touch_center_z - touch_pcb_size / 2
        ])
            cube([
                touch_pcb_size,
                touch_pcb_t,
                touch_pcb_size
            ]);

    color("gold", 0.40)
        translate([
            touch_pad_center_x,
            pcb_outer_face_y + 0.02,
            touch_pad_center_z
        ])
            rotate([-90, 0, 0])
                cylinder(h = 0.08, d = touch_pad_d);

    color("silver", 0.55)
        translate([
            touch_solder_center_x - touch_solder_x_size / 2,
            touch_panel_inner_y,
            touch_solder_center_z - touch_solder_z_size / 2
        ])
            cube([
                touch_solder_x_size,
                touch_solder_clearance,
                touch_solder_z_size
            ]);

    color("silver", 0.45)
        touch_sensor_lower_left_point(case_h + 0.05) {
            rotate([90, 0, 0])
                cylinder(
                    h = wall + touch_pcb_t + 1.2,
                    d = 2.5
                );

            rotate([90, 0, 0])
                cylinder(
                    h = touch_m25_head_h,
                    d = 5.0
                );
        }

    color("silver", 0.45)
        touch_sensor_upper_right_point(
            pcb_inner_face_y - touch_m25_head_h
        ) {
            rotate([-90, 0, 0])
                cylinder(
                    h = touch_m25_head_h,
                    d = 5.0
                );

            translate([0, touch_m25_head_h - 0.05, 0])
                rotate([-90, 0, 0])
                    cylinder(
                        h =
                            touch_pcb_t
                            + touch_right_pilot_depth
                            + 0.2,
                        d = 2.5
                    );
        }
}

module speaker_preview() {
    color("gray", 0.35)
        translate([
            speaker_center_x - speaker_w / 2,
            speaker_center_y - speaker_h / 2,
            wall + 0.15
        ])
            rounded_cube([speaker_w, speaker_h, 4.0], 2.0);
}

module assembly() {
    base();

    translate([0, 0, case_d + assembly_lid_lift])
        lid();

    oled_preview();
    pi_zero_preview();
    amp_preview();
    usb_c_header_preview();
    touch_sensor_preview();
    speaker_preview();
}

// -----------------------------------------------------------------------------
// Output
// -----------------------------------------------------------------------------

if (part == "base") {
    base();
}

if (part == "lid") {
    lid();
}

if (part == "assembly") {
    assembly();
}
