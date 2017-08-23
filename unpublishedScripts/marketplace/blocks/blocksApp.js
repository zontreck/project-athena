///
/// blocksApp.js
/// A tablet app for downloading 3D assets from Google Blocks
/// 
/// Author: Elisa Lupin-Jimenez
/// Copyright High Fidelity 2017
///
/// Licensed under the Apache 2.0 License
/// See accompanying license file or http://apache.org/
///
/// All assets are under CC Attribution Non-Commerical
/// http://creativecommons.org/licenses/
///

(function () {
	var APP_NAME = "BLOCKS";
	var APP_URL = "https://vr.google.com/objects/";
	var APP_ICON = "https://hifi-content.s3.amazonaws.com/elisalj/blocks/blocks-i.svg";
	var APP_ICON_ACTIVE = "https://hifi-content.s3.amazonaws.com/elisalj/blocks/blocks-a.svg";

    try {
        print("Current Interface version: " + Window.checkVersion());
    } catch(err) {
        print("Outdated Interface version does not support Blocks");
        APP_URL = "https://hifi-content.s3.amazonaws.com/elisalj/blocks/updateToBlocks.html";
    }

	var tablet = Tablet.getTablet("com.highfidelity.interface.tablet.system");
	var button = tablet.addButton({
	    icon: APP_ICON,
        activeIcon: APP_ICON_ACTIVE,
		text: APP_NAME
	});

	function onClicked() {
		tablet.gotoWebScreen(APP_URL, "", true);
	}
	button.clicked.connect(onClicked);

	function onScreenChanged(type, url) {
	    if (url !== null) {
	        tabletButton.editProperties({ isActive: true });

	        if (!shown) {
	            // hook up to the event bridge
	            tablet.webEventReceived.connect(onWebEventReceived);
	        }
	        shown = true;
	    } else {
	        tabletButton.editProperties({ isActive: false });

	        if (shown) {
	            // disconnect from the event bridge
	            tablet.webEventReceived.disconnect(onWebEventReceived);
	        }
	        shown = false;
	    }
	}

	function cleanup() {
		tablet.removeButton(button);
	}

	Script.scriptEnding.connect(cleanup);
}());
