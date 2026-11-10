//Maya ASCII 2026 scene
//Name: rig_pins.ma
//Last modified: Tue, Oct 27, 2026 02:39:49 PM
//Codeset: 1252
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
fileInfo "product" "Maya 2026";
fileInfo "version" "2026";
fileInfo "cutIdentifier" "202510291147-60ec9eda33";
fileInfo "osv" "Windows 11 Pro v2009 (Build: 26200)";
fileInfo "UUID" "DE8593F8-43C1-827F-DA23-5EB97FD0086C";
createNode transform -s -n "persp";
	rename -uid "5A3C7A74-41D0-9188-AA29-5793C530F06A";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 28 21 28 ;
	setAttr ".r" -type "double3" -27.938352729602379 44.999999999999972 -5.172681101354183e-14 ;
createNode camera -s -n "perspShape" -p "persp";
	rename -uid "216E0EFB-40B5-26D0-5911-BBA2248075BA";
	setAttr -k off ".v" no;
	setAttr ".fl" 34.999999999999993;
	setAttr ".coi" 44.82186966202994;
	setAttr ".imn" -type "string" "persp";
	setAttr ".den" -type "string" "persp_depth";
	setAttr ".man" -type "string" "persp_mask";
	setAttr ".hc" -type "string" "viewSet -p %camera";
createNode transform -s -n "top";
	rename -uid "A8C5C150-4CFD-DD21-7700-B2AA4CA436E3";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 1000.1 0 ;
	setAttr ".r" -type "double3" -90 0 0 ;
createNode camera -s -n "topShape" -p "top";
	rename -uid "2FC289D1-48E3-94DA-7952-EEA91ED5278C";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "top";
	setAttr ".den" -type "string" "top_depth";
	setAttr ".man" -type "string" "top_mask";
	setAttr ".hc" -type "string" "viewSet -t %camera";
	setAttr ".o" yes;
createNode transform -s -n "front";
	rename -uid "1FC214EF-47CF-8E21-EA06-2FAFFC5D2427";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 0 1000.1 ;
createNode camera -s -n "frontShape" -p "front";
	rename -uid "FE7A9F34-4CF7-B620-AD51-39892494C929";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "front";
	setAttr ".den" -type "string" "front_depth";
	setAttr ".man" -type "string" "front_mask";
	setAttr ".hc" -type "string" "viewSet -f %camera";
	setAttr ".o" yes;
createNode transform -s -n "side";
	rename -uid "C38474D4-4C8D-29DA-A80A-3D8E6F35D0C8";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 1000.1 0 0 ;
	setAttr ".r" -type "double3" 0 90 0 ;
createNode camera -s -n "sideShape" -p "side";
	rename -uid "E718FD4D-45C4-4C23-827F-EEBC87A5220E";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "side";
	setAttr ".den" -type "string" "side_depth";
	setAttr ".man" -type "string" "side_mask";
	setAttr ".hc" -type "string" "viewSet -s %camera";
	setAttr ".o" yes;
createNode transform -n "pinPlane";
	rename -uid "91D3B031-4D5F-E4EB-8693-63A04B13FADB";
	setAttr ".t" -type "double3" 1 2 3 ;
createNode mesh -n "pinPlaneShape" -p "pinPlane";
	rename -uid "42BD5409-4CB6-CAE7-45B9-5F8BC80739D4";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 5 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "e[36:39]";
	setAttr ".gtag[1].gtagnm" -type "string" "front";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 4 "e[0]" "e[2]" "e[4]" "e[6]";
	setAttr ".gtag[2].gtagnm" -type "string" "left";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 4 "e[1]" "e[10]" "e[19]" "e[28]";
	setAttr ".gtag[3].gtagnm" -type "string" "right";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 4 "e[8]" "e[17]" "e[26]" "e[35]";
	setAttr ".gtag[4].gtagnm" -type "string" "rim";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 10 "e[0:2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[17]" "e[19]" "e[26]" "e[28]" "e[35:39]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 25 ".uvst[0].uvsp[0:24]" -type "float2" 0 0 0.25 0 0.5 0
		 0.75 0 1 0 0 0.25 0.25 0.25 0.5 0.25 0.75 0.25 1 0.25 0 0.5 0.25 0.5 0.5 0.5 0.75
		 0.5 1 0.5 0 0.75 0.25 0.75 0.5 0.75 0.75 0.75 1 0.75 0 1 0.25 1 0.5 1 0.75 1 1 1;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 25 ".vt[0:24]"  -2 0 2 -1 0 2 0 0 2 1 0 2 2 0 2 -2 0 1 -1 0 1
		 0 0 1 1 0 1 2 0 1 -2 0 0 -1 0 0 0 0 0 1 0 0 2 0 0 -2 0 -1 -1 0 -1 0 0 -1 1 0 -1 2 0 -1
		 -2 0 -2 -1 0 -2 0 0 -2 1 0 -2 2 0 -2;
	setAttr -s 40 ".ed[0:39]"  0 1 0 0 5 0 1 2 0 1 6 1 2 3 0 2 7 1 3 4 0
		 3 8 1 4 9 0 5 6 1 5 10 0 6 7 1 6 11 1 7 8 1 7 12 1 8 9 1 8 13 1 9 14 0 10 11 1 10 15 0
		 11 12 1 11 16 1 12 13 1 12 17 1 13 14 1 13 18 1 14 19 0 15 16 1 15 20 0 16 17 1 16 21 1
		 17 18 1 17 22 1 18 19 1 18 23 1 19 24 0 20 21 0 21 22 0 22 23 0 23 24 0;
	setAttr -s 16 -ch 64 ".fc[0:15]" -type "polyFaces" 
		f 4 0 3 -10 -2
		mu 0 4 0 1 6 5
		f 4 2 5 -12 -4
		mu 0 4 1 2 7 6
		f 4 4 7 -14 -6
		mu 0 4 2 3 8 7
		f 4 6 8 -16 -8
		mu 0 4 3 4 9 8
		f 4 9 12 -19 -11
		mu 0 4 5 6 11 10
		f 4 11 14 -21 -13
		mu 0 4 6 7 12 11
		f 4 13 16 -23 -15
		mu 0 4 7 8 13 12
		f 4 15 17 -25 -17
		mu 0 4 8 9 14 13
		f 4 18 21 -28 -20
		mu 0 4 10 11 16 15
		f 4 20 23 -30 -22
		mu 0 4 11 12 17 16
		f 4 22 25 -32 -24
		mu 0 4 12 13 18 17
		f 4 24 26 -34 -26
		mu 0 4 13 14 19 18
		f 4 27 30 -37 -29
		mu 0 4 15 16 21 20
		f 4 29 32 -38 -31
		mu 0 4 16 17 22 21
		f 4 31 34 -39 -33
		mu 0 4 17 18 23 22
		f 4 33 35 -40 -35
		mu 0 4 18 19 24 23;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "fol";
	rename -uid "196B7DCB-4533-49A8-027E-FCAA37B8515B";
createNode follicle -n "folShape" -p "fol";
	rename -uid "B4491A4D-4E12-C9B2-C6DE-9FB7CB813DE5";
	setAttr -k off ".v";
	setAttr ".pu" 0.3;
	setAttr ".pv" 0.7;
	setAttr -s 2 ".sts[0:1]"  0 1 3 1 0.2 3;
	setAttr -s 2 ".cws[0:1]"  0 1 3 1 0.2 3;
	setAttr -s 2 ".ats[0:1]"  0 1 3 1 0.2 3;
createNode transform -n "pinnedA";
	rename -uid "AEAA2687-4F49-720C-EC58-C094091D6398";
createNode locator -n "pinnedAShape" -p "pinnedA";
	rename -uid "AC8A2F5F-46B0-362F-8AC8-BB8B534A6323";
	setAttr -k off ".v";
createNode transform -n "pinnedB";
	rename -uid "650FC652-4A75-1C38-32D8-85BF54671045";
createNode locator -n "pinnedBShape" -p "pinnedB";
	rename -uid "3E27D056-4755-6FDC-5D8A-AEB8E9976A50";
	setAttr -k off ".v";
createNode transform -n "proxSheet";
	rename -uid "7E157C8B-45C3-E233-8A99-52A748A5B5A2";
	setAttr ".rp" -type "double3" 0 0 10 ;
	setAttr ".sp" -type "double3" 0 0 10 ;
createNode mesh -n "proxSheetShape" -p "proxSheet";
	rename -uid "9C657A0E-408D-B373-4B17-98A0170D954D";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "proxSheetShapeOrig" -p "proxSheet";
	rename -uid "498B3F8F-4E12-33E5-6590-37B2A02623F4";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "e[36:39]";
	setAttr ".gtag[1].gtagnm" -type "string" "front";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 4 "e[0]" "e[2]" "e[4]" "e[6]";
	setAttr ".gtag[2].gtagnm" -type "string" "left";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 4 "e[1]" "e[10]" "e[19]" "e[28]";
	setAttr ".gtag[3].gtagnm" -type "string" "right";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 4 "e[8]" "e[17]" "e[26]" "e[35]";
	setAttr ".gtag[4].gtagnm" -type "string" "rim";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 10 "e[0:2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[17]" "e[19]" "e[26]" "e[28]" "e[35:39]";
	setAttr ".gtag[5].gtagnm" -type "string" "sheetBend";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "vtx[0:9]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 25 ".uvst[0].uvsp[0:24]" -type "float2" 0 0 0.25 0 0.5 0
		 0.75 0 1 0 0 0.25 0.25 0.25 0.5 0.25 0.75 0.25 1 0.25 0 0.5 0.25 0.5 0.5 0.5 0.75
		 0.5 1 0.5 0 0.75 0.25 0.75 0.5 0.75 0.75 0.75 1 0.75 0 1 0.25 1 0.5 1 0.75 1 1 1;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 25 ".pt[0:24]" -type "float3"  0 0 10 0 0 10 0 0 10 0 0 
		10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 
		0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10;
	setAttr -s 25 ".vt[0:24]"  -2 0 2 -1 0 2 0 0 2 1 0 2 2 0 2 -2 0 1 -1 0 1
		 0 0 1 1 0 1 2 0 1 -2 0 0 -1 0 0 0 0 0 1 0 0 2 0 0 -2 0 -1 -1 0 -1 0 0 -1 1 0 -1 2 0 -1
		 -2 0 -2 -1 0 -2 0 0 -2 1 0 -2 2 0 -2;
	setAttr -s 40 ".ed[0:39]"  0 1 0 0 5 0 1 2 0 1 6 1 2 3 0 2 7 1 3 4 0
		 3 8 1 4 9 0 5 6 1 5 10 0 6 7 1 6 11 1 7 8 1 7 12 1 8 9 1 8 13 1 9 14 0 10 11 1 10 15 0
		 11 12 1 11 16 1 12 13 1 12 17 1 13 14 1 13 18 1 14 19 0 15 16 1 15 20 0 16 17 1 16 21 1
		 17 18 1 17 22 1 18 19 1 18 23 1 19 24 0 20 21 0 21 22 0 22 23 0 23 24 0;
	setAttr -s 16 -ch 64 ".fc[0:15]" -type "polyFaces" 
		f 4 0 3 -10 -2
		mu 0 4 0 1 6 5
		f 4 2 5 -12 -4
		mu 0 4 1 2 7 6
		f 4 4 7 -14 -6
		mu 0 4 2 3 8 7
		f 4 6 8 -16 -8
		mu 0 4 3 4 9 8
		f 4 9 12 -19 -11
		mu 0 4 5 6 11 10
		f 4 11 14 -21 -13
		mu 0 4 6 7 12 11
		f 4 13 16 -23 -15
		mu 0 4 7 8 13 12
		f 4 15 17 -25 -17
		mu 0 4 8 9 14 13
		f 4 18 21 -28 -20
		mu 0 4 10 11 16 15
		f 4 20 23 -30 -22
		mu 0 4 11 12 17 16
		f 4 22 25 -32 -24
		mu 0 4 12 13 18 17
		f 4 24 26 -34 -26
		mu 0 4 13 14 19 18
		f 4 27 30 -37 -29
		mu 0 4 15 16 21 20
		f 4 29 32 -38 -31
		mu 0 4 16 17 22 21
		f 4 31 34 -39 -33
		mu 0 4 17 18 23 22
		f 4 33 35 -40 -35
		mu 0 4 18 19 24 23;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "sheetBendHandle";
	rename -uid "E66B18F0-4E6E-DE56-635F-839AF885FFFB";
	setAttr ".rp" -type "double3" 0 0 11.5 ;
	setAttr ".sp" -type "double3" 0 0 11.5 ;
createNode clusterHandle -n "sheetBendHandleShape" -p "sheetBendHandle";
	rename -uid "6229845E-4B51-FE75-9D6F-8B94D56CF08B";
	setAttr ".ihi" 0;
	setAttr -k off ".v";
	setAttr ".or" -type "double3" 0 0 11.5 ;
createNode transform -n "pinnedC";
	rename -uid "106E9A19-48F3-B25A-D03F-1285D0FDE8EF";
createNode locator -n "pinnedCShape" -p "pinnedC";
	rename -uid "C0D3FE8E-4060-8D9A-35FE-A3AEDC2F05CA";
	setAttr -k off ".v";
createNode lightLinker -s -n "lightLinker1";
	rename -uid "F257374F-4761-41A7-B322-009E10ACDBB3";
	setAttr -s 2 ".lnk";
	setAttr -s 2 ".slnk";
createNode displayLayerManager -n "layerManager";
	rename -uid "7D9AFE39-4F3C-6BA0-580B-249C48BCBD4B";
createNode displayLayer -n "defaultLayer";
	rename -uid "DE1782EA-42CB-FDDE-DDC7-D299C9EC6647";
	setAttr ".ufem" -type "stringArray" 0  ;
createNode renderLayerManager -n "renderLayerManager";
	rename -uid "B328D26B-4A16-B1DA-BF3E-9687A7FA4B9C";
createNode renderLayer -n "defaultRenderLayer";
	rename -uid "08DC8B12-456B-E3FC-4AED-C5B26B4FBA03";
	setAttr ".g" yes;
createNode uvPin -n "uvp";
	rename -uid "95D21564-4715-2B4D-4A3A-439A227FBE07";
	setAttr -s 2 ".coord[0:1]" -type "double2" 0.6 0.25 0.9 0.9;
	setAttr ".nrm" 1;
	setAttr ".tng" 0;
	setAttr -s 2 ".omat";
createNode cluster -n "sheetBend";
	rename -uid "BA69ED88-417E-66DB-9D87-80B43ED3263D";
	setAttr ".ip[0].gtg" -type "string" "sheetBend";
	setAttr ".gm[0]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
createNode proximityPin -n "prox";
	rename -uid "E0EFB33B-4058-9EC8-E4B6-3DB9BD74CE05";
	setAttr ".imat[0]" -type "matrix" 0.91645952550798937 0.091952665971431716 -0.38941834230865041 0
		 -0.020864418700063062 0.98289398754707169 0.18298657129998705 0 0.39958305036372527 -0.15957479896450785 0.90270109637546003 0
		 0.59999999999999998 0.29999999999999999 9.0999999999999996 1;
createNode script -n "uiConfigurationScriptNode";
	rename -uid "E87852D4-4E41-1C3B-56E5-34A518CD740C";
	setAttr ".b" -type "string" "// Maya Mel UI Configuration File.\n// No UI generated in batch mode.\n";
	setAttr ".st" 3;
createNode script -n "sceneConfigurationScriptNode";
	rename -uid "795AADBA-4B53-43C9-1FF1-89AC6E62E5C4";
	setAttr ".b" -type "string" "playbackOptions -min 1 -max 120 -ast 1 -aet 200 ";
	setAttr ".st" 6;
select -ne :time1;
	setAttr ".o" 1;
	setAttr ".unw" 1;
select -ne :hardwareRenderingGlobals;
	setAttr ".otfna" -type "stringArray" 22 "NURBS Curves" "NURBS Surfaces" "Polygons" "Subdiv Surface" "Particles" "Particle Instance" "Fluids" "Strokes" "Image Planes" "UI" "Lights" "Cameras" "Locators" "Joints" "IK Handles" "Deformers" "Motion Trails" "Components" "Hair Systems" "Follicles" "Misc. UI" "Ornaments"  ;
	setAttr ".otfva" -type "Int32Array" 22 0 1 1 1 1 1
		 1 1 1 0 0 0 0 0 0 0 0 0
		 0 0 0 0 ;
	setAttr ".fprt" yes;
	setAttr ".rtfm" 1;
select -ne :renderPartition;
	setAttr -s 2 ".st";
select -ne :renderGlobalsList1;
select -ne :defaultShaderList1;
	setAttr -s 6 ".s";
select -ne :postProcessList1;
	setAttr -s 2 ".p";
select -ne :defaultRenderingList1;
select -ne :standardSurface1;
	setAttr ".bc" -type "float3" 0.40000001 0.40000001 0.40000001 ;
	setAttr ".sr" 0.5;
select -ne :openPBR_shader1;
	setAttr ".bc" -type "float3" 0.40000001 0.40000001 0.40000001 ;
	setAttr ".sr" 0.5;
select -ne :initialShadingGroup;
	setAttr -s 2 ".dsm";
	setAttr ".ro" yes;
select -ne :initialParticleSE;
	setAttr ".ro" yes;
select -ne :defaultRenderGlobals;
	addAttr -ci true -h true -sn "dss" -ln "defaultSurfaceShader" -dt "string";
	setAttr ".dss" -type "string" "openPBR_shader1";
select -ne :defaultResolution;
	setAttr ".pa" 1;
select -ne :defaultColorMgtGlobals;
	setAttr ".cfe" yes;
	setAttr ".cfp" -type "string" "<MAYA_RESOURCES>/OCIO-configs/Maya2022-default/config.ocio";
	setAttr ".vtn" -type "string" "ACES 1.0 SDR-video (sRGB)";
	setAttr ".vn" -type "string" "ACES 1.0 SDR-video";
	setAttr ".dn" -type "string" "sRGB";
	setAttr ".wsn" -type "string" "ACEScg";
	setAttr ".otn" -type "string" "ACES 1.0 SDR-video (sRGB)";
	setAttr ".potn" -type "string" "ACES 1.0 SDR-video (sRGB)";
select -ne :hardwareRenderGlobals;
	setAttr ".ctrs" 256;
	setAttr ".btrs" 512;
connectAttr "folShape.ot" "fol.t";
connectAttr "folShape.or" "fol.r";
connectAttr "pinPlaneShape.o" "folShape.inm";
connectAttr "pinPlaneShape.wm" "folShape.iwm";
connectAttr "uvp.omat[0]" "pinnedA.opm";
connectAttr "uvp.omat[1]" "pinnedB.opm";
connectAttr "sheetBend.og[0]" "proxSheetShape.i";
connectAttr "prox.omat[0]" "pinnedC.opm";
relationship "link" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
connectAttr "layerManager.dli[0]" "defaultLayer.id";
connectAttr "renderLayerManager.rlmi[0]" "defaultRenderLayer.rlid";
connectAttr "pinPlaneShape.w" "uvp.curgeom";
connectAttr "proxSheetShapeOrig.w" "sheetBend.ip[0].ig";
connectAttr "proxSheetShapeOrig.o" "sheetBend.orggeom[0]";
connectAttr "sheetBendHandle.wm" "sheetBend.ma";
connectAttr "sheetBendHandleShape.x" "sheetBend.x";
connectAttr "proxSheetShape.w" "prox.curgeom";
connectAttr "proxSheetShapeOrig.w" "prox.orggeom";
connectAttr "defaultRenderLayer.msg" ":defaultRenderingList1.r" -na;
connectAttr "pinPlaneShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "proxSheetShape.iog" ":initialShadingGroup.dsm" -na;
// End of rig_pins.ma
