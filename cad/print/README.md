# Print files

All parts are already oriented the right way up for the print bed, just load them into Bambu Studio.

Sorted by colour. 01 to 10 are grey, including all parts that sit inside and are not visible. 11 is the name plate with a colour change. Everything from 12 on is black.

| File | Colour | Support | Note |
| --- | --- | --- | --- |
| 01_top_case_grey | grey | no | upside down, top face on the bed, brim |
| 02_base_plate_grey | grey | only under the motor mount | brim |
| 03_bearing_cover_grey | grey | no | carries the RC522, sits loose on the ring |
| 04_bearing_pin_grey | grey | no | glued into the bearing cover |
| 04b_thrust_ring_grey | grey | no | Ø36 x 1.6 mm, glued centred under the hub, smooth bed side down. The hub then rests flat instead of on the pin tip and no longer tilts under belt tension |
| 05_belt_pulley_grey | grey | no | flange on the bed, groove 6 mm high, hub up |
| 05c_belt_cage_grey | grey | only under the lower lip | upside down, glued to the base around the pulley, opening towards the platter |
| 05d_tensioner_set_grey | grey | no | base with two bosses for M2 heat-set inserts, slider with slot and printed 6 mm axle, roller with 9 mm groove, clamp ring. Only two short M2 (6 to 8 mm) needed |
| 06_tonearm_stop_ring_grey | grey | no | sits under the lid |
| 06b_switch_lever_shims_grey | grey | no | four shims 4 mm wide, 0.6 / 1.0 / 1.4 / 1.8 mm thick (shortest is thinnest), glue the right one onto the lever of the tonearm micro switch |
| 06b_switch_lever_shim_5x6x1-8_grey | grey | no | single 5 x 6 mm shim, 1.8 mm thick, for the same lever |
| 07_control_bracket_buttons_encoder_grey | grey | no | screwed to the two button bosses from below |
| 07b_encoder_support_grey | grey | no | supports the encoder from below, adjust the height to your measurement before printing |
| 08_shuffle_holder_grey | grey | no | holds the 12x12 button, glued under the lid |
| 09_shuffle_plunger_grey | grey | no | glued into the puck from below |
| 10_shuffle_drill_template_grey | grey | no | drilling aid, not part of the device |
| 11_name_plate_AMS_two_colour.3mf | black, text grey | no | with AMS, plate and text are two parts of one object |
| 11_name_plate_grey_black_from_1-0mm | grey, black from 1.0 mm | no | version without AMS, filament change at 1.0 mm |
| 11b_name_plate_text_inlay_grey | grey | no | only needed if the 3MF does not load correctly, load together with 11 as one object |
| 12_platter_black | black | no | flat disc, top face on the bed, plugs onto the hub from above |
| 12b_centre_spindle_black | black | no | glued 1.5 mm deep into the centre of the platter |
| 12c_hub_belt_groove_black | black | no | top face on the bed, sits on the bearing pin, belt groove 9 mm high |
| 13_record_black | black | no | top face on the bed |
| 14_tonearm_post_black | black | yes, under the disc | |
| 15_tonearm_black_PAUSE_8-6mm | black | no | pause at 8.6 mm, insert an M8 nut |
| 16_tonearm_axle_pin_black | black | no | two diameters, use the one that fits |
| 17_tonearm_rest_black | black | no | |
| 18_knob_black | black | no | ironing, layer height 0.08 |
| 19_button_cap_previous_left_black | black | no | left hole, symbol side on the bed, layer height 0.08, long side with symbol points up in the device |
| 19_button_cap_next_right_black | black | no | right hole, same as above |
| 20_shuffle_puck_black | black | no | shuffle button at the back left, recessed symbol, no ironing |
| 21_foot_1 to 4_black | black | no | |

## Settings

Layer height 0.12 mm for top case and platter, variable layer height on, seam at the back, outer wall 60 to 80 mm/s, precise wall on. Ironing only on the top surface.

Name plate without AMS: in the Bambu Studio preview move the slider to the first layer above 1.0 mm and add a filament change to black with a right click. Up to 1.0 mm it prints grey, that becomes the bottom of the engraving.

## Assembly

1. Screw the RC522 under the bearing cover first (M2). Put the cover on the ring in the base plate, the tab goes into the notch at the back. The cable runs down inside to the window at the back of the ring.
2. Glue the bearing pin into the cover. Put the hub on the pin and lay the belt around hub and pulley. Only then put the top case on, the hub goes through the hole in the lid. Finally plug the platter on from above, the three pins go into the holes in the hub. Glue the centre spindle into the recess in the middle of the platter first.
3. Tonearm post through the lid from above, glue the stop ring onto the flat from below.
4. Push the micro switch onto the two pins under the lid, lever up, lever end under the hole.
5. The tonearm head holds the M8 nut. More weight: an M8 set screw (6 mm) or solder in the nut hole.
6. Fix the rest from below with an M2 screw through the lid.
7. Control bracket: put the two 12x12 buttons with their legs into the slots, the legs come out at the bottom and are soldered there. Push the encoder through the hole in the platform from below and tighten its nut on top. Then screw the bracket to the bosses next to the button holes with two M2 screws from below. The encoder neck sits 9 mm lower than when mounted directly to the lid, so the knob floats just above the lid.
8. All screws are M2 and go straight into the 1.8 mm holes.
9. Drill the shuffle hole. Put the template on the back left corner, the lips rest against the outside of the left and back wall. Drill 7 mm through the guide. The centre is 22 mm from the left and 22 mm from the back.
10. Put the 12x12 button with its legs into the slots of the shuffle holder. Glue the holder to the lid under the hole, the button plunger points up to the hole.
11. Push the plunger through the hole from below and glue it into the socket of the puck. The flange at the bottom keeps it from falling out. Check that the button clicks before gluing.
12. Shuffle button to GPIO 17 and GND (J8). The left cap is Previous (GPIO 32), the right one is Next (GPIO 33).
13. Glue the name plate to the front at the bottom left, about 12 mm from the left edge and 4 mm above the bottom. Small dots of super glue on the back, otherwise it squeezes out at the edges.
14. Encoder support: it stands on the base right under the encoder and pushes against the KY-040 board from below, so the platform does not give way when the knob is pressed. Before printing, measure from the bottom edge of the top case to the underside of the KY-040 board and enter that as the height (Z) in Bambu Studio, the file is 30 mm. Put a strip of double sided foam tape (about 1 mm) on top so it sticks to the board with a little preload. The recess on top leaves room for the solder joints. It gets pressed up when the base is screwed on.
15. Tensioner: put two M2 heat-set inserts into the base and glue it to the floor next to the belt, long side towards the belt. Put the slider on, push the roller onto the axle and press the ring on top. Push the slider against the belt until it is just tight enough, then tighten with two short M2 through the slot into the inserts. Best close to the pulley on the side where the belt sags.
16. Thrust ring: take the hub off and turn it over. Put a 6 mm drill bit into the bore as a centring pin, slide the ring over it smooth side up and glue it to the underside of the hub with three or four small dots of super glue. When dry, put a little vaseline or oil on the bearing cover around the pin and put the hub back. The platter now sits about 1 mm higher with about 2 mm clearance above the lid. Only set the tensioner so far that the belt does not sag.
