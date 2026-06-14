extends SceneTree

func print_node(node: Node, depth: int = 0) -> void:
	var indent := "  ".repeat(depth)
	if node is Node3D:
		print(indent, node.name, " class=", node.get_class(), " transform=", node.transform)
	else:
		print(indent, node.name, " class=", node.get_class())
	if node is GridMap:
		print(indent, "cell_size=", node.cell_size, " center_x=", node.cell_center_x,
			" center_z=", node.cell_center_z, " used_cells=", node.get_used_cells().size())
		var cells: Array[Vector3i] = node.get_used_cells()
		for index in range(min(cells.size(), 5)):
			var cell: Vector3i = cells[index]
			print(indent, "cell=", cell, " orientation=", node.get_cell_item_orientation(cell),
				" local=", node.map_to_local(cell))
	for child in node.get_children():
		print_node(child, depth + 1)

func _initialize() -> void:
	var packed := load("res://Godot/Scenes/MainServer.tscn") as PackedScene
	var root := packed.instantiate()
	get_root().add_child(root)
	var terrain_scene := root.get_node("TerrainScene") as Node3D
	var grid := root.get_node("TerrainScene/TerrainGrid/Terrain") as GridMap
	print("terrain_scene_global=", terrain_scene.global_transform)
	print("grid_global=", grid.global_transform)
	for cell in [Vector3i(79, 0, 0), Vector3i(0, 0, 0), Vector3i(79, 0, 79), Vector3i(0, 0, 79)]:
		var godot_world := grid.to_global(grid.map_to_local(cell))
		print("cell=", cell, " godot=", godot_world,
			" source=", Vector3(godot_world.x, -godot_world.y, -godot_world.z))
	quit()
