cylinder_res = 50;
tol = .2;

footswitch_radius = .46 * 25.4;
jack_radius = 9.525;

width = 40;
length = 90;
height = 25;
shell_thickness = 3;

jack_height = 10;
jack_pos1 = 60;
jack_pos2 = jack_pos1 - 10;

footswitch_pos = 22;

pedal();

module pedal()
{
	difference()
	{

		// the outside of the pedals
		cube([ width, length, height ]);

		union()
		{
			// cut out the inside of the pedal
			translate([ shell_thickness, shell_thickness, 0 ])
			cube([ width - shell_thickness * 2, length - shell_thickness * 2, height - shell_thickness ]);

			// left and right jacks
			translate([ 0, jack_pos1, jack_height ])
			rotate([ 0, 90, 0 ])
			cylinder(shell_thickness, jack_radius / 2, jack_radius / 2, $fn = cylinder_res);

			translate([ width - shell_thickness, jack_pos2, jack_height ])
			rotate([ 0, 90, 0 ])
			cylinder(shell_thickness, jack_radius / 2, jack_radius / 2, $fn = cylinder_res);

			// footswitch
			translate([ width / 2, footswitch_pos, height - shell_thickness ])
			cylinder(shell_thickness, footswitch_radius / 2, footswitch_radius / 2, $fn = cylinder_res);
		}
	}
}
