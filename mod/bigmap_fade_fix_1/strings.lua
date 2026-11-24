function data()
return {
	en = {
		["mod_name"] = "Big Maps: street fade fix",
		["mod_desc"] = "Fixes streets (and some other objects) whitening, or turning into multicoloured speckles under Vulkan, when the camera is far from the map centre -- most visible with the line manager's grey layer (CommonAPI's \"white layer\").\n\n"
			.. "Cause: the game fades these objects out by the camera's distance to their bounding box, but when the renderer supplies no box the shaders fall back to a point at the map centre, so the fade follows the camera's distance to the centre (from ~10 km) instead of the distance to the street.\n\n"
			.. "Fix: without a box, the distance is measured to the object itself. Objects that have a box are unchanged. Five vertex shaders replaced; purely visual, client-side, safe to add or remove at any time. Works on vanilla Megalomaniac maps and on larger maps.",
	},
	fr = {
		["mod_name"] = "Big Maps: street fade fix",
		["mod_desc"] = "Corrige le blanchiment des rues (et de quelques autres objets), ou leur transformation en points multicolores sous Vulkan, quand la caméra est loin du centre de la carte -- surtout visible avec le calque gris du gestionnaire de lignes (le « white layer » de CommonAPI).\n\n"
			.. "Cause : le jeu estompe ces objets selon la distance de la caméra à leur boîte englobante, mais quand le rendu ne fournit pas de boîte, les shaders se rabattent sur un point au centre de la carte : l'estompage suit alors la distance de la caméra au centre (dès ~10 km) au lieu de la distance à la rue.\n\n"
			.. "Correctif : sans boîte, la distance est mesurée jusqu'à l'objet lui-même. Les objets qui ont une boîte ne changent pas. Cinq shaders de sommets remplacés ; purement visuel, côté client, ajout et retrait sans risque à tout moment. Fonctionne sur les cartes Megalomaniac vanilla comme sur les cartes plus grandes.",
	},
	de = {
		["mod_name"] = "Big Maps: street fade fix",
	},
}
end
