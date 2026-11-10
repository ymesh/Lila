//Maya ASCII 2026 scene
//Name: rig_deformers.ma
//Last modified: Tue, Oct 27, 2026 01:52:46 PM
//Codeset: 1252
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
fileInfo "product" "Maya 2026";
fileInfo "version" "2026";
fileInfo "cutIdentifier" "202510291147-60ec9eda33";
fileInfo "osv" "Windows 11 Pro v2009 (Build: 26200)";
fileInfo "UUID" "0365827F-4B52-5A79-66FF-79BB8638CB98";
createNode transform -s -n "persp";
	rename -uid "E2DD51A3-46F8-4D41-B1DD-67906429ED22";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 28 21 28 ;
	setAttr ".r" -type "double3" -27.938352729602379 44.999999999999972 -5.172681101354183e-14 ;
createNode camera -s -n "perspShape" -p "persp";
	rename -uid "EFC00E01-495F-27AA-4F4C-21A5226E2CDA";
	setAttr -k off ".v" no;
	setAttr ".fl" 34.999999999999993;
	setAttr ".coi" 44.82186966202994;
	setAttr ".imn" -type "string" "persp";
	setAttr ".den" -type "string" "persp_depth";
	setAttr ".man" -type "string" "persp_mask";
	setAttr ".hc" -type "string" "viewSet -p %camera";
createNode transform -s -n "top";
	rename -uid "C86C3D99-4C84-7932-87E7-2CB93397BEA9";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 1000.1 0 ;
	setAttr ".r" -type "double3" -90 0 0 ;
createNode camera -s -n "topShape" -p "top";
	rename -uid "E36FDD76-499D-79F0-3B52-56A304286F2E";
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
	rename -uid "AE668643-4349-0E37-7961-CF8AFD3437B1";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 0 1000.1 ;
createNode camera -s -n "frontShape" -p "front";
	rename -uid "75E08A2A-4ACF-0DCD-4F53-1EB70F24EFED";
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
	rename -uid "3A44CD8A-4DE7-94C6-0745-F191C937422F";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 1000.1 0 0 ;
	setAttr ".r" -type "double3" 0 90 0 ;
createNode camera -s -n "sideShape" -p "side";
	rename -uid "50378046-4E32-9FBD-3694-8B9E83346EF9";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "side";
	setAttr ".den" -type "string" "side_depth";
	setAttr ".man" -type "string" "side_mask";
	setAttr ".hc" -type "string" "viewSet -s %camera";
	setAttr ".o" yes;
createNode transform -n "clusterPlane";
	rename -uid "8661C531-47D5-553F-D630-AB97CF86F880";
createNode mesh -n "clusterPlaneShape" -p "clusterPlane";
	rename -uid "81238231-4D13-6276-3B1A-FBBC8415CFE6";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "clusterPlaneShapeOrig" -p "clusterPlane";
	rename -uid "246BFBEA-4B3C-7836-0419-C18BC2FE9DF8";
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
	setAttr ".gtag[5].gtagnm" -type "string" "clu";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "vtx[0:12]";
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
createNode transform -n "cluHandle";
	rename -uid "76866991-4023-03CE-6D2C-9F8BBD630439";
	setAttr ".rp" -type "double3" 0 0 1 ;
	setAttr ".sp" -type "double3" 0 0 1 ;
createNode clusterHandle -n "cluHandleShape" -p "cluHandle";
	rename -uid "1A6E97E8-4FD1-5C43-48BB-64824F30BAD2";
	setAttr ".ihi" 0;
	setAttr -k off ".v";
	setAttr ".or" -type "double3" 0 0 1 ;
createNode transform -n "clusterPlaneB";
	rename -uid "BF8FD0C3-41DE-4194-9340-AFA898A0FAF7";
	setAttr ".rp" -type "double3" 0 0 10 ;
	setAttr ".sp" -type "double3" 0 0 10 ;
createNode mesh -n "clusterPlaneBShape" -p "clusterPlaneB";
	rename -uid "DEBC8EA9-4368-1504-61D4-3DBBE658369F";
	setAttr -k off ".v";
	setAttr -s 2 ".iog[0].og";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "clusterPlaneBShapeOrig" -p "clusterPlaneB";
	rename -uid "2557F792-4FA9-1B66-57CE-829FDF8A9185";
	setAttr -k off ".v";
	setAttr ".io" yes;
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
createNode transform -n "cluBHandle";
	rename -uid "9562D238-4D3E-51F4-0124-11989C52BB85";
	setAttr ".rp" -type "double3" 0 0 12 ;
	setAttr ".sp" -type "double3" 0 0 12 ;
createNode clusterHandle -n "cluBHandleShape" -p "cluBHandle";
	rename -uid "68B76054-44F7-EB19-B08B-F79C9154425E";
	setAttr ".ihi" 0;
	setAttr -k off ".v";
	setAttr ".or" -type "double3" 0 0 12 ;
createNode transform -n "latCube";
	rename -uid "7A50B9F1-4627-B25A-7D4B-C8AFAEDEBA3B";
	setAttr ".rp" -type "double3" 10 0 0 ;
	setAttr ".sp" -type "double3" 10 0 0 ;
createNode mesh -n "latCubeShape" -p "latCube";
	rename -uid "3A04E12B-4945-F39D-EB57-3283551ACE4B";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "latCubeShapeOrig" -p "latCube";
	rename -uid "E9668F33-48EC-9C30-8747-CE800EAE6504";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[18:26]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[27:35]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0:8]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[45:53]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[36:44]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[9:17]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 76 ".uvst[0].uvsp[0:75]" -type "float2" 0.33000001 0 0.44111112
		 0 0.55222225 0 0.66333336 0 0.33000001 0.083333336 0.44111112 0.083333336 0.55222225
		 0.083333336 0.66333336 0.083333336 0.33000001 0.16666667 0.44111112 0.16666667 0.55222225
		 0.16666667 0.66333336 0.16666667 0.33000001 0.25 0.44111112 0.25 0.55222225 0.25
		 0.66333336 0.25 0.33000001 0.33333334 0.44111112 0.33333334 0.55222225 0.33333334
		 0.66333336 0.33333334 0.33000001 0.41666669 0.44111112 0.41666669 0.55222225 0.41666669
		 0.66333336 0.41666669 0.33000001 0.5 0.44111112 0.5 0.55222225 0.5 0.66333336 0.5
		 0.33000001 0.58333331 0.44111112 0.58333331 0.55222225 0.58333331 0.66333336 0.58333331
		 0.33000001 0.66666663 0.44111112 0.66666663 0.55222225 0.66666663 0.66333336 0.66666663
		 0.33000001 0.74999994 0.44111112 0.74999994 0.55222225 0.74999994 0.66333336 0.74999994
		 0.33000001 0.83333325 0.44111112 0.83333325 0.55222225 0.83333325 0.66333336 0.83333325
		 0.33000001 0.91666657 0.44111112 0.91666657 0.55222225 0.91666657 0.66333336 0.91666657
		 0.33000001 0.99999988 0.44111112 0.99999988 0.55222225 0.99999988 0.66333336 0.99999988
		 1 0 0.91666669 0 0.83333337 0 1 0.083333336 0.91666669 0.083333336 0.83333337 0.083333336
		 1 0.16666667 0.91666669 0.16666667 0.83333337 0.16666667 1 0.25 0.91666669 0.25 0.83333337
		 0.25 0 0 0.083333336 0 0.16666667 0 0 0.083333336 0.083333336 0.083333336 0.16666667
		 0.083333336 0 0.16666667 0.083333336 0.16666667 0.16666667 0.16666667 0 0.25 0.083333336
		 0.25 0.16666667 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 56 ".pt[0:55]" -type "float3"  10 0 0 10 0 0 10 0 0 10 0 
		0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 
		0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 
		10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 
		0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 
		0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0;
	setAttr -s 56 ".vt[0:55]"  -1 -1 1 -0.33333331 -1 1 0.33333337 -1 1
		 1 -1 1 -1 -0.33333331 1 -0.33333331 -0.33333331 1 0.33333337 -0.33333331 1 1 -0.33333331 1
		 -1 0.33333337 1 -0.33333331 0.33333337 1 0.33333337 0.33333337 1 1 0.33333337 1 -1 1 1
		 -0.33333331 1 1 0.33333337 1 1 1 1 1 -1 1 0.33333331 -0.33333331 1 0.33333331 0.33333337 1 0.33333331
		 1 1 0.33333331 -1 1 -0.33333337 -0.33333331 1 -0.33333337 0.33333337 1 -0.33333337
		 1 1 -0.33333337 -1 1 -1 -0.33333331 1 -1 0.33333337 1 -1 1 1 -1 -1 0.33333331 -1
		 -0.33333331 0.33333331 -1 0.33333337 0.33333331 -1 1 0.33333331 -1 -1 -0.33333337 -1
		 -0.33333331 -0.33333337 -1 0.33333337 -0.33333337 -1 1 -0.33333337 -1 -1 -1 -1 -0.33333331 -1 -1
		 0.33333337 -1 -1 1 -1 -1 -1 -1 -0.33333331 -0.33333331 -1 -0.33333331 0.33333337 -1 -0.33333331
		 1 -1 -0.33333331 -1 -1 0.33333337 -0.33333331 -1 0.33333337 0.33333337 -1 0.33333337
		 1 -1 0.33333337 1 -0.33333331 -0.33333331 1 -0.33333331 0.33333337 1 0.33333337 -0.33333331
		 1 0.33333337 0.33333337 -1 -0.33333331 -0.33333331 -1 -0.33333331 0.33333337 -1 0.33333337 -0.33333331
		 -1 0.33333337 0.33333337;
	setAttr -s 108 ".ed[0:107]"  0 1 0 1 2 0 2 3 0 4 5 1 5 6 1 6 7 1 8 9 1
		 9 10 1 10 11 1 12 13 0 13 14 0 14 15 0 16 17 1 17 18 1 18 19 1 20 21 1 21 22 1 22 23 1
		 24 25 0 25 26 0 26 27 0 28 29 1 29 30 1 30 31 1 32 33 1 33 34 1 34 35 1 36 37 0 37 38 0
		 38 39 0 40 41 1 41 42 1 42 43 1 44 45 1 45 46 1 46 47 1 0 4 0 1 5 1 2 6 1 3 7 0 4 8 0
		 5 9 1 6 10 1 7 11 0 8 12 0 9 13 1 10 14 1 11 15 0 12 16 0 13 17 1 14 18 1 15 19 0
		 16 20 0 17 21 1 18 22 1 19 23 0 20 24 0 21 25 1 22 26 1 23 27 0 24 28 0 25 29 1 26 30 1
		 27 31 0 28 32 0 29 33 1 30 34 1 31 35 0 32 36 0 33 37 1 34 38 1 35 39 0 36 40 0 37 41 1
		 38 42 1 39 43 0 40 44 0 41 45 1 42 46 1 43 47 0 44 0 0 45 1 1 46 2 1 47 3 0 35 48 1
		 48 49 1 49 7 1 31 50 1 50 51 1 51 11 1 43 48 1 47 49 1 48 50 1 49 51 1 50 23 1 51 19 1
		 32 52 1 52 53 1 53 4 1 28 54 1 54 55 1 55 8 1 40 52 1 44 53 1 52 54 1 53 55 1 54 20 1
		 55 16 1;
	setAttr -s 54 -ch 216 ".fc[0:53]" -type "polyFaces" 
		f 4 0 37 -4 -37
		mu 0 4 0 1 5 4
		f 4 1 38 -5 -38
		mu 0 4 1 2 6 5
		f 4 2 39 -6 -39
		mu 0 4 2 3 7 6
		f 4 3 41 -7 -41
		mu 0 4 4 5 9 8
		f 4 4 42 -8 -42
		mu 0 4 5 6 10 9
		f 4 5 43 -9 -43
		mu 0 4 6 7 11 10
		f 4 6 45 -10 -45
		mu 0 4 8 9 13 12
		f 4 7 46 -11 -46
		mu 0 4 9 10 14 13
		f 4 8 47 -12 -47
		mu 0 4 10 11 15 14
		f 4 9 49 -13 -49
		mu 0 4 12 13 17 16
		f 4 10 50 -14 -50
		mu 0 4 13 14 18 17
		f 4 11 51 -15 -51
		mu 0 4 14 15 19 18
		f 4 12 53 -16 -53
		mu 0 4 16 17 21 20
		f 4 13 54 -17 -54
		mu 0 4 17 18 22 21
		f 4 14 55 -18 -55
		mu 0 4 18 19 23 22
		f 4 15 57 -19 -57
		mu 0 4 20 21 25 24
		f 4 16 58 -20 -58
		mu 0 4 21 22 26 25
		f 4 17 59 -21 -59
		mu 0 4 22 23 27 26
		f 4 18 61 -22 -61
		mu 0 4 24 25 29 28
		f 4 19 62 -23 -62
		mu 0 4 25 26 30 29
		f 4 20 63 -24 -63
		mu 0 4 26 27 31 30
		f 4 21 65 -25 -65
		mu 0 4 28 29 33 32
		f 4 22 66 -26 -66
		mu 0 4 29 30 34 33
		f 4 23 67 -27 -67
		mu 0 4 30 31 35 34
		f 4 24 69 -28 -69
		mu 0 4 32 33 37 36
		f 4 25 70 -29 -70
		mu 0 4 33 34 38 37
		f 4 26 71 -30 -71
		mu 0 4 34 35 39 38
		f 4 27 73 -31 -73
		mu 0 4 36 37 41 40
		f 4 28 74 -32 -74
		mu 0 4 37 38 42 41
		f 4 29 75 -33 -75
		mu 0 4 38 39 43 42
		f 4 30 77 -34 -77
		mu 0 4 40 41 45 44
		f 4 31 78 -35 -78
		mu 0 4 41 42 46 45
		f 4 32 79 -36 -79
		mu 0 4 42 43 47 46
		f 4 33 81 -1 -81
		mu 0 4 44 45 49 48
		f 4 34 82 -2 -82
		mu 0 4 45 46 50 49
		f 4 35 83 -3 -83
		mu 0 4 46 47 51 50
		f 4 -76 -72 84 -91
		mu 0 4 53 52 55 56
		f 4 -80 90 85 -92
		mu 0 4 54 53 56 57
		f 4 -84 91 86 -40
		mu 0 4 3 54 57 7
		f 4 -85 -68 87 -93
		mu 0 4 56 55 58 59
		f 4 -86 92 88 -94
		mu 0 4 57 56 59 60
		f 4 -87 93 89 -44
		mu 0 4 7 57 60 11
		f 4 -88 -64 -60 -95
		mu 0 4 59 58 61 62
		f 4 -89 94 -56 -96
		mu 0 4 60 59 62 63
		f 4 -90 95 -52 -48
		mu 0 4 11 60 63 15
		f 4 72 102 -97 68
		mu 0 4 64 65 68 67
		f 4 76 103 -98 -103
		mu 0 4 65 66 69 68
		f 4 80 36 -99 -104
		mu 0 4 66 0 4 69
		f 4 96 104 -100 64
		mu 0 4 67 68 71 70
		f 4 97 105 -101 -105
		mu 0 4 68 69 72 71
		f 4 98 40 -102 -106
		mu 0 4 69 4 8 72
		f 4 99 106 56 60
		mu 0 4 70 71 74 73
		f 4 100 107 52 -107
		mu 0 4 71 72 75 74
		f 4 101 44 48 -108
		mu 0 4 72 8 12 75;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "latLattice";
	rename -uid "4FAE22D4-4F7E-055F-6835-0EBAA187617D";
	setAttr ".t" -type "double3" 10 0 0 ;
	setAttr ".s" -type "double3" 2 2 2 ;
createNode lattice -n "latLatticeShape" -p "latLattice";
	rename -uid "EF3728CC-490E-E772-F534-49A57E53A3B7";
	setAttr -k off ".v";
	setAttr ".td" 3;
	setAttr ".cc" -type "lattice" 2 3 2 12 -0.5 -0.5 -0.5 0.5 -0.5
		 -0.5 -0.5 0 -0.5 0.80000000000000004 0 -0.29999999999999999 -0.5 0.5 -0.5 0.5 0.5
		 -0.5 -0.5 -0.5 0.5 0.5 -0.5 0.5 -0.5 0 0.5 0.5 0 0.5 -0.69999999999999996 0.90000000000000002
		 0.5 0.5 0.5 0.5 ;
createNode transform -n "latBase";
	rename -uid "21C02EFF-4AE6-E418-B6A0-ADB2E47C8A31";
	setAttr ".t" -type "double3" 10 0 0 ;
	setAttr ".s" -type "double3" 2 2 2 ;
createNode baseLattice -n "latBaseShape" -p "latBase";
	rename -uid "A606EC55-4F9B-4422-4BD8-5D9B330415D7";
	setAttr ".ihi" 0;
	setAttr -k off ".v";
createNode transform -n "bendCube";
	rename -uid "953C6F0F-41D8-5705-6523-B7B8A5725744";
	setAttr ".rp" -type "double3" 0 0 -10 ;
	setAttr ".sp" -type "double3" 0 0 -10 ;
createNode mesh -n "bendCubeShape" -p "bendCube";
	rename -uid "F3135E09-49C4-62C7-03F0-5C924BCFE745";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "bendCubeShapeOrig" -p "bendCube";
	rename -uid "E9E08DFE-456E-A8CE-FE5A-23A952CB6962";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[32:47]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[48:63]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0:15]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[80:95]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[64:79]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[16:31]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 125 ".uvst[0].uvsp[0:124]" -type "float2" 0.33000001 0 0.41333336
		 0 0.4966667 0 0.58000004 0 0.66333336 0 0.33000001 0.0625 0.41333336 0.0625 0.4966667
		 0.0625 0.58000004 0.0625 0.66333336 0.0625 0.33000001 0.125 0.41333336 0.125 0.4966667
		 0.125 0.58000004 0.125 0.66333336 0.125 0.33000001 0.1875 0.41333336 0.1875 0.4966667
		 0.1875 0.58000004 0.1875 0.66333336 0.1875 0.33000001 0.25 0.41333336 0.25 0.4966667
		 0.25 0.58000004 0.25 0.66333336 0.25 0.33000001 0.3125 0.41333336 0.3125 0.4966667
		 0.3125 0.58000004 0.3125 0.66333336 0.3125 0.33000001 0.375 0.41333336 0.375 0.4966667
		 0.375 0.58000004 0.375 0.66333336 0.375 0.33000001 0.4375 0.41333336 0.4375 0.4966667
		 0.4375 0.58000004 0.4375 0.66333336 0.4375 0.33000001 0.5 0.41333336 0.5 0.4966667
		 0.5 0.58000004 0.5 0.66333336 0.5 0.33000001 0.5625 0.41333336 0.5625 0.4966667 0.5625
		 0.58000004 0.5625 0.66333336 0.5625 0.33000001 0.625 0.41333336 0.625 0.4966667 0.625
		 0.58000004 0.625 0.66333336 0.625 0.33000001 0.6875 0.41333336 0.6875 0.4966667 0.6875
		 0.58000004 0.6875 0.66333336 0.6875 0.33000001 0.75 0.41333336 0.75 0.4966667 0.75
		 0.58000004 0.75 0.66333336 0.75 0.33000001 0.8125 0.41333336 0.8125 0.4966667 0.8125
		 0.58000004 0.8125 0.66333336 0.8125 0.33000001 0.875 0.41333336 0.875 0.4966667 0.875
		 0.58000004 0.875 0.66333336 0.875 0.33000001 0.9375 0.41333336 0.9375 0.4966667 0.9375
		 0.58000004 0.9375 0.66333336 0.9375 0.33000001 1 0.41333336 1 0.4966667 1 0.58000004
		 1 0.66333336 1 1 0 0.9375 0 0.875 0 0.8125 0 1 0.0625 0.9375 0.0625 0.875 0.0625
		 0.8125 0.0625 1 0.125 0.9375 0.125 0.875 0.125 0.8125 0.125 1 0.1875 0.9375 0.1875
		 0.875 0.1875 0.8125 0.1875 1 0.25 0.9375 0.25 0.875 0.25 0.8125 0.25 0 0 0.0625 0
		 0.125 0 0.1875 0 0 0.0625 0.0625 0.0625 0.125 0.0625 0.1875 0.0625 0 0.125 0.0625
		 0.125 0.125 0.125 0.1875 0.125 0 0.1875 0.0625 0.1875 0.125 0.1875 0.1875 0.1875
		 0 0.25 0.0625 0.25 0.125 0.25 0.1875 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 98 ".pt[0:97]" -type "float3"  0 0 -10 0 0 -10 0 0 -10 0 
		0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 
		-10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 
		0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 
		0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 
		-10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 
		0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 
		0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 
		-10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 
		0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 
		0 -10 0 0 -10;
	setAttr -s 98 ".vt[0:97]"  -1 -1 1 -0.5 -1 1 0 -1 1 0.5 -1 1 1 -1 1
		 -1 -0.5 1 -0.5 -0.5 1 0 -0.5 1 0.5 -0.5 1 1 -0.5 1 -1 0 1 -0.5 0 1 0 0 1 0.5 0 1
		 1 0 1 -1 0.5 1 -0.5 0.5 1 0 0.5 1 0.5 0.5 1 1 0.5 1 -1 1 1 -0.5 1 1 0 1 1 0.5 1 1
		 1 1 1 -1 1 0.5 -0.5 1 0.5 0 1 0.5 0.5 1 0.5 1 1 0.5 -1 1 0 -0.5 1 0 0 1 0 0.5 1 0
		 1 1 0 -1 1 -0.5 -0.5 1 -0.5 0 1 -0.5 0.5 1 -0.5 1 1 -0.5 -1 1 -1 -0.5 1 -1 0 1 -1
		 0.5 1 -1 1 1 -1 -1 0.5 -1 -0.5 0.5 -1 0 0.5 -1 0.5 0.5 -1 1 0.5 -1 -1 0 -1 -0.5 0 -1
		 0 0 -1 0.5 0 -1 1 0 -1 -1 -0.5 -1 -0.5 -0.5 -1 0 -0.5 -1 0.5 -0.5 -1 1 -0.5 -1 -1 -1 -1
		 -0.5 -1 -1 0 -1 -1 0.5 -1 -1 1 -1 -1 -1 -1 -0.5 -0.5 -1 -0.5 0 -1 -0.5 0.5 -1 -0.5
		 1 -1 -0.5 -1 -1 0 -0.5 -1 0 0 -1 0 0.5 -1 0 1 -1 0 -1 -1 0.5 -0.5 -1 0.5 0 -1 0.5
		 0.5 -1 0.5 1 -1 0.5 1 -0.5 -0.5 1 -0.5 0 1 -0.5 0.5 1 0 -0.5 1 0 0 1 0 0.5 1 0.5 -0.5
		 1 0.5 0 1 0.5 0.5 -1 -0.5 -0.5 -1 -0.5 0 -1 -0.5 0.5 -1 0 -0.5 -1 0 0 -1 0 0.5 -1 0.5 -0.5
		 -1 0.5 0 -1 0.5 0.5;
	setAttr -s 192 ".ed";
	setAttr ".ed[0:165]"  0 1 0 1 2 0 2 3 0 3 4 0 5 6 1 6 7 1 7 8 1 8 9 1 10 11 1
		 11 12 1 12 13 1 13 14 1 15 16 1 16 17 1 17 18 1 18 19 1 20 21 0 21 22 0 22 23 0 23 24 0
		 25 26 1 26 27 1 27 28 1 28 29 1 30 31 1 31 32 1 32 33 1 33 34 1 35 36 1 36 37 1 37 38 1
		 38 39 1 40 41 0 41 42 0 42 43 0 43 44 0 45 46 1 46 47 1 47 48 1 48 49 1 50 51 1 51 52 1
		 52 53 1 53 54 1 55 56 1 56 57 1 57 58 1 58 59 1 60 61 0 61 62 0 62 63 0 63 64 0 65 66 1
		 66 67 1 67 68 1 68 69 1 70 71 1 71 72 1 72 73 1 73 74 1 75 76 1 76 77 1 77 78 1 78 79 1
		 0 5 0 1 6 1 2 7 1 3 8 1 4 9 0 5 10 0 6 11 1 7 12 1 8 13 1 9 14 0 10 15 0 11 16 1
		 12 17 1 13 18 1 14 19 0 15 20 0 16 21 1 17 22 1 18 23 1 19 24 0 20 25 0 21 26 1 22 27 1
		 23 28 1 24 29 0 25 30 0 26 31 1 27 32 1 28 33 1 29 34 0 30 35 0 31 36 1 32 37 1 33 38 1
		 34 39 0 35 40 0 36 41 1 37 42 1 38 43 1 39 44 0 40 45 0 41 46 1 42 47 1 43 48 1 44 49 0
		 45 50 0 46 51 1 47 52 1 48 53 1 49 54 0 50 55 0 51 56 1 52 57 1 53 58 1 54 59 0 55 60 0
		 56 61 1 57 62 1 58 63 1 59 64 0 60 65 0 61 66 1 62 67 1 63 68 1 64 69 0 65 70 0 66 71 1
		 67 72 1 68 73 1 69 74 0 70 75 0 71 76 1 72 77 1 73 78 1 74 79 0 75 0 0 76 1 1 77 2 1
		 78 3 1 79 4 0 59 80 1 80 81 1 81 82 1 82 9 1 54 83 1 83 84 1 84 85 1 85 14 1 49 86 1
		 86 87 1 87 88 1 88 19 1 69 80 1 74 81 1 79 82 1 80 83 1 81 84 1 82 85 1 83 86 1 84 87 1
		 85 88 1 86 39 1;
	setAttr ".ed[166:191]" 87 34 1 88 29 1 55 89 1 89 90 1 90 91 1 91 5 1 50 92 1
		 92 93 1 93 94 1 94 10 1 45 95 1 95 96 1 96 97 1 97 15 1 65 89 1 70 90 1 75 91 1 89 92 1
		 90 93 1 91 94 1 92 95 1 93 96 1 94 97 1 95 35 1 96 30 1 97 25 1;
	setAttr -s 96 -ch 384 ".fc[0:95]" -type "polyFaces" 
		f 4 0 65 -5 -65
		mu 0 4 0 1 6 5
		f 4 1 66 -6 -66
		mu 0 4 1 2 7 6
		f 4 2 67 -7 -67
		mu 0 4 2 3 8 7
		f 4 3 68 -8 -68
		mu 0 4 3 4 9 8
		f 4 4 70 -9 -70
		mu 0 4 5 6 11 10
		f 4 5 71 -10 -71
		mu 0 4 6 7 12 11
		f 4 6 72 -11 -72
		mu 0 4 7 8 13 12
		f 4 7 73 -12 -73
		mu 0 4 8 9 14 13
		f 4 8 75 -13 -75
		mu 0 4 10 11 16 15
		f 4 9 76 -14 -76
		mu 0 4 11 12 17 16
		f 4 10 77 -15 -77
		mu 0 4 12 13 18 17
		f 4 11 78 -16 -78
		mu 0 4 13 14 19 18
		f 4 12 80 -17 -80
		mu 0 4 15 16 21 20
		f 4 13 81 -18 -81
		mu 0 4 16 17 22 21
		f 4 14 82 -19 -82
		mu 0 4 17 18 23 22
		f 4 15 83 -20 -83
		mu 0 4 18 19 24 23
		f 4 16 85 -21 -85
		mu 0 4 20 21 26 25
		f 4 17 86 -22 -86
		mu 0 4 21 22 27 26
		f 4 18 87 -23 -87
		mu 0 4 22 23 28 27
		f 4 19 88 -24 -88
		mu 0 4 23 24 29 28
		f 4 20 90 -25 -90
		mu 0 4 25 26 31 30
		f 4 21 91 -26 -91
		mu 0 4 26 27 32 31
		f 4 22 92 -27 -92
		mu 0 4 27 28 33 32
		f 4 23 93 -28 -93
		mu 0 4 28 29 34 33
		f 4 24 95 -29 -95
		mu 0 4 30 31 36 35
		f 4 25 96 -30 -96
		mu 0 4 31 32 37 36
		f 4 26 97 -31 -97
		mu 0 4 32 33 38 37
		f 4 27 98 -32 -98
		mu 0 4 33 34 39 38
		f 4 28 100 -33 -100
		mu 0 4 35 36 41 40
		f 4 29 101 -34 -101
		mu 0 4 36 37 42 41
		f 4 30 102 -35 -102
		mu 0 4 37 38 43 42
		f 4 31 103 -36 -103
		mu 0 4 38 39 44 43
		f 4 32 105 -37 -105
		mu 0 4 40 41 46 45
		f 4 33 106 -38 -106
		mu 0 4 41 42 47 46
		f 4 34 107 -39 -107
		mu 0 4 42 43 48 47
		f 4 35 108 -40 -108
		mu 0 4 43 44 49 48
		f 4 36 110 -41 -110
		mu 0 4 45 46 51 50
		f 4 37 111 -42 -111
		mu 0 4 46 47 52 51
		f 4 38 112 -43 -112
		mu 0 4 47 48 53 52
		f 4 39 113 -44 -113
		mu 0 4 48 49 54 53
		f 4 40 115 -45 -115
		mu 0 4 50 51 56 55
		f 4 41 116 -46 -116
		mu 0 4 51 52 57 56
		f 4 42 117 -47 -117
		mu 0 4 52 53 58 57
		f 4 43 118 -48 -118
		mu 0 4 53 54 59 58
		f 4 44 120 -49 -120
		mu 0 4 55 56 61 60
		f 4 45 121 -50 -121
		mu 0 4 56 57 62 61
		f 4 46 122 -51 -122
		mu 0 4 57 58 63 62
		f 4 47 123 -52 -123
		mu 0 4 58 59 64 63
		f 4 48 125 -53 -125
		mu 0 4 60 61 66 65
		f 4 49 126 -54 -126
		mu 0 4 61 62 67 66
		f 4 50 127 -55 -127
		mu 0 4 62 63 68 67
		f 4 51 128 -56 -128
		mu 0 4 63 64 69 68
		f 4 52 130 -57 -130
		mu 0 4 65 66 71 70
		f 4 53 131 -58 -131
		mu 0 4 66 67 72 71
		f 4 54 132 -59 -132
		mu 0 4 67 68 73 72
		f 4 55 133 -60 -133
		mu 0 4 68 69 74 73
		f 4 56 135 -61 -135
		mu 0 4 70 71 76 75
		f 4 57 136 -62 -136
		mu 0 4 71 72 77 76
		f 4 58 137 -63 -137
		mu 0 4 72 73 78 77
		f 4 59 138 -64 -138
		mu 0 4 73 74 79 78
		f 4 60 140 -1 -140
		mu 0 4 75 76 81 80
		f 4 61 141 -2 -141
		mu 0 4 76 77 82 81
		f 4 62 142 -3 -142
		mu 0 4 77 78 83 82
		f 4 63 143 -4 -143
		mu 0 4 78 79 84 83
		f 4 -129 -124 144 -157
		mu 0 4 86 85 89 90
		f 4 -134 156 145 -158
		mu 0 4 87 86 90 91
		f 4 -139 157 146 -159
		mu 0 4 88 87 91 92
		f 4 -144 158 147 -69
		mu 0 4 4 88 92 9
		f 4 -145 -119 148 -160
		mu 0 4 90 89 93 94
		f 4 -146 159 149 -161
		mu 0 4 91 90 94 95
		f 4 -147 160 150 -162
		mu 0 4 92 91 95 96
		f 4 -148 161 151 -74
		mu 0 4 9 92 96 14
		f 4 -149 -114 152 -163
		mu 0 4 94 93 97 98
		f 4 -150 162 153 -164
		mu 0 4 95 94 98 99
		f 4 -151 163 154 -165
		mu 0 4 96 95 99 100
		f 4 -152 164 155 -79
		mu 0 4 14 96 100 19
		f 4 -153 -109 -104 -166
		mu 0 4 98 97 101 102
		f 4 -154 165 -99 -167
		mu 0 4 99 98 102 103
		f 4 -155 166 -94 -168
		mu 0 4 100 99 103 104
		f 4 -156 167 -89 -84
		mu 0 4 19 100 104 24
		f 4 124 180 -169 119
		mu 0 4 105 106 110 109
		f 4 129 181 -170 -181
		mu 0 4 106 107 111 110
		f 4 134 182 -171 -182
		mu 0 4 107 108 112 111
		f 4 139 64 -172 -183
		mu 0 4 108 0 5 112
		f 4 168 183 -173 114
		mu 0 4 109 110 114 113
		f 4 169 184 -174 -184
		mu 0 4 110 111 115 114
		f 4 170 185 -175 -185
		mu 0 4 111 112 116 115
		f 4 171 69 -176 -186
		mu 0 4 112 5 10 116
		f 4 172 186 -177 109
		mu 0 4 113 114 118 117
		f 4 173 187 -178 -187
		mu 0 4 114 115 119 118
		f 4 174 188 -179 -188
		mu 0 4 115 116 120 119
		f 4 175 74 -180 -189
		mu 0 4 116 10 15 120
		f 4 176 189 99 104
		mu 0 4 117 118 122 121
		f 4 177 190 94 -190
		mu 0 4 118 119 123 122
		f 4 178 191 89 -191
		mu 0 4 119 120 124 123
		f 4 179 79 84 -192
		mu 0 4 120 15 20 124;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "bndHandle";
	rename -uid "4D84B8D4-48FA-2EA2-0F2C-EEBF8D899D06";
	setAttr ".t" -type "double3" 0 0 -10 ;
	setAttr ".smd" 7;
createNode deformBend -n "bndHandleShape" -p "bndHandle";
	rename -uid "E3FEF905-456F-B1CF-1FEB-C98E33AEC73F";
	setAttr -k off ".v";
	setAttr ".hw" 1.1;
createNode transform -n "twistCube";
	rename -uid "A7351E41-4DD7-DFAA-F7B2-2EBE6B0C6B5A";
	setAttr ".rp" -type "double3" 10 0 -10 ;
	setAttr ".sp" -type "double3" 10 0 -10 ;
createNode mesh -n "twistCubeShape" -p "twistCube";
	rename -uid "2EE7CA5D-4162-8A46-093F-B1A90A822EFC";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "twistCubeShapeOrig" -p "twistCube";
	rename -uid "EE5D68A6-4681-013D-E273-569A49273C90";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[32:47]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[48:63]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0:15]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[80:95]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[64:79]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[16:31]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 125 ".uvst[0].uvsp[0:124]" -type "float2" 0.33000001 0 0.41333336
		 0 0.4966667 0 0.58000004 0 0.66333336 0 0.33000001 0.0625 0.41333336 0.0625 0.4966667
		 0.0625 0.58000004 0.0625 0.66333336 0.0625 0.33000001 0.125 0.41333336 0.125 0.4966667
		 0.125 0.58000004 0.125 0.66333336 0.125 0.33000001 0.1875 0.41333336 0.1875 0.4966667
		 0.1875 0.58000004 0.1875 0.66333336 0.1875 0.33000001 0.25 0.41333336 0.25 0.4966667
		 0.25 0.58000004 0.25 0.66333336 0.25 0.33000001 0.3125 0.41333336 0.3125 0.4966667
		 0.3125 0.58000004 0.3125 0.66333336 0.3125 0.33000001 0.375 0.41333336 0.375 0.4966667
		 0.375 0.58000004 0.375 0.66333336 0.375 0.33000001 0.4375 0.41333336 0.4375 0.4966667
		 0.4375 0.58000004 0.4375 0.66333336 0.4375 0.33000001 0.5 0.41333336 0.5 0.4966667
		 0.5 0.58000004 0.5 0.66333336 0.5 0.33000001 0.5625 0.41333336 0.5625 0.4966667 0.5625
		 0.58000004 0.5625 0.66333336 0.5625 0.33000001 0.625 0.41333336 0.625 0.4966667 0.625
		 0.58000004 0.625 0.66333336 0.625 0.33000001 0.6875 0.41333336 0.6875 0.4966667 0.6875
		 0.58000004 0.6875 0.66333336 0.6875 0.33000001 0.75 0.41333336 0.75 0.4966667 0.75
		 0.58000004 0.75 0.66333336 0.75 0.33000001 0.8125 0.41333336 0.8125 0.4966667 0.8125
		 0.58000004 0.8125 0.66333336 0.8125 0.33000001 0.875 0.41333336 0.875 0.4966667 0.875
		 0.58000004 0.875 0.66333336 0.875 0.33000001 0.9375 0.41333336 0.9375 0.4966667 0.9375
		 0.58000004 0.9375 0.66333336 0.9375 0.33000001 1 0.41333336 1 0.4966667 1 0.58000004
		 1 0.66333336 1 1 0 0.9375 0 0.875 0 0.8125 0 1 0.0625 0.9375 0.0625 0.875 0.0625
		 0.8125 0.0625 1 0.125 0.9375 0.125 0.875 0.125 0.8125 0.125 1 0.1875 0.9375 0.1875
		 0.875 0.1875 0.8125 0.1875 1 0.25 0.9375 0.25 0.875 0.25 0.8125 0.25 0 0 0.0625 0
		 0.125 0 0.1875 0 0 0.0625 0.0625 0.0625 0.125 0.0625 0.1875 0.0625 0 0.125 0.0625
		 0.125 0.125 0.125 0.1875 0.125 0 0.1875 0.0625 0.1875 0.125 0.1875 0.1875 0.1875
		 0 0.25 0.0625 0.25 0.125 0.25 0.1875 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 98 ".pt[0:97]" -type "float3"  10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10 
		10 0 -10 10 0 -10 10 0 -10 10 0 -10 10 0 -10;
	setAttr -s 98 ".vt[0:97]"  -1 -1 1 -0.5 -1 1 0 -1 1 0.5 -1 1 1 -1 1
		 -1 -0.5 1 -0.5 -0.5 1 0 -0.5 1 0.5 -0.5 1 1 -0.5 1 -1 0 1 -0.5 0 1 0 0 1 0.5 0 1
		 1 0 1 -1 0.5 1 -0.5 0.5 1 0 0.5 1 0.5 0.5 1 1 0.5 1 -1 1 1 -0.5 1 1 0 1 1 0.5 1 1
		 1 1 1 -1 1 0.5 -0.5 1 0.5 0 1 0.5 0.5 1 0.5 1 1 0.5 -1 1 0 -0.5 1 0 0 1 0 0.5 1 0
		 1 1 0 -1 1 -0.5 -0.5 1 -0.5 0 1 -0.5 0.5 1 -0.5 1 1 -0.5 -1 1 -1 -0.5 1 -1 0 1 -1
		 0.5 1 -1 1 1 -1 -1 0.5 -1 -0.5 0.5 -1 0 0.5 -1 0.5 0.5 -1 1 0.5 -1 -1 0 -1 -0.5 0 -1
		 0 0 -1 0.5 0 -1 1 0 -1 -1 -0.5 -1 -0.5 -0.5 -1 0 -0.5 -1 0.5 -0.5 -1 1 -0.5 -1 -1 -1 -1
		 -0.5 -1 -1 0 -1 -1 0.5 -1 -1 1 -1 -1 -1 -1 -0.5 -0.5 -1 -0.5 0 -1 -0.5 0.5 -1 -0.5
		 1 -1 -0.5 -1 -1 0 -0.5 -1 0 0 -1 0 0.5 -1 0 1 -1 0 -1 -1 0.5 -0.5 -1 0.5 0 -1 0.5
		 0.5 -1 0.5 1 -1 0.5 1 -0.5 -0.5 1 -0.5 0 1 -0.5 0.5 1 0 -0.5 1 0 0 1 0 0.5 1 0.5 -0.5
		 1 0.5 0 1 0.5 0.5 -1 -0.5 -0.5 -1 -0.5 0 -1 -0.5 0.5 -1 0 -0.5 -1 0 0 -1 0 0.5 -1 0.5 -0.5
		 -1 0.5 0 -1 0.5 0.5;
	setAttr -s 192 ".ed";
	setAttr ".ed[0:165]"  0 1 0 1 2 0 2 3 0 3 4 0 5 6 1 6 7 1 7 8 1 8 9 1 10 11 1
		 11 12 1 12 13 1 13 14 1 15 16 1 16 17 1 17 18 1 18 19 1 20 21 0 21 22 0 22 23 0 23 24 0
		 25 26 1 26 27 1 27 28 1 28 29 1 30 31 1 31 32 1 32 33 1 33 34 1 35 36 1 36 37 1 37 38 1
		 38 39 1 40 41 0 41 42 0 42 43 0 43 44 0 45 46 1 46 47 1 47 48 1 48 49 1 50 51 1 51 52 1
		 52 53 1 53 54 1 55 56 1 56 57 1 57 58 1 58 59 1 60 61 0 61 62 0 62 63 0 63 64 0 65 66 1
		 66 67 1 67 68 1 68 69 1 70 71 1 71 72 1 72 73 1 73 74 1 75 76 1 76 77 1 77 78 1 78 79 1
		 0 5 0 1 6 1 2 7 1 3 8 1 4 9 0 5 10 0 6 11 1 7 12 1 8 13 1 9 14 0 10 15 0 11 16 1
		 12 17 1 13 18 1 14 19 0 15 20 0 16 21 1 17 22 1 18 23 1 19 24 0 20 25 0 21 26 1 22 27 1
		 23 28 1 24 29 0 25 30 0 26 31 1 27 32 1 28 33 1 29 34 0 30 35 0 31 36 1 32 37 1 33 38 1
		 34 39 0 35 40 0 36 41 1 37 42 1 38 43 1 39 44 0 40 45 0 41 46 1 42 47 1 43 48 1 44 49 0
		 45 50 0 46 51 1 47 52 1 48 53 1 49 54 0 50 55 0 51 56 1 52 57 1 53 58 1 54 59 0 55 60 0
		 56 61 1 57 62 1 58 63 1 59 64 0 60 65 0 61 66 1 62 67 1 63 68 1 64 69 0 65 70 0 66 71 1
		 67 72 1 68 73 1 69 74 0 70 75 0 71 76 1 72 77 1 73 78 1 74 79 0 75 0 0 76 1 1 77 2 1
		 78 3 1 79 4 0 59 80 1 80 81 1 81 82 1 82 9 1 54 83 1 83 84 1 84 85 1 85 14 1 49 86 1
		 86 87 1 87 88 1 88 19 1 69 80 1 74 81 1 79 82 1 80 83 1 81 84 1 82 85 1 83 86 1 84 87 1
		 85 88 1 86 39 1;
	setAttr ".ed[166:191]" 87 34 1 88 29 1 55 89 1 89 90 1 90 91 1 91 5 1 50 92 1
		 92 93 1 93 94 1 94 10 1 45 95 1 95 96 1 96 97 1 97 15 1 65 89 1 70 90 1 75 91 1 89 92 1
		 90 93 1 91 94 1 92 95 1 93 96 1 94 97 1 95 35 1 96 30 1 97 25 1;
	setAttr -s 96 -ch 384 ".fc[0:95]" -type "polyFaces" 
		f 4 0 65 -5 -65
		mu 0 4 0 1 6 5
		f 4 1 66 -6 -66
		mu 0 4 1 2 7 6
		f 4 2 67 -7 -67
		mu 0 4 2 3 8 7
		f 4 3 68 -8 -68
		mu 0 4 3 4 9 8
		f 4 4 70 -9 -70
		mu 0 4 5 6 11 10
		f 4 5 71 -10 -71
		mu 0 4 6 7 12 11
		f 4 6 72 -11 -72
		mu 0 4 7 8 13 12
		f 4 7 73 -12 -73
		mu 0 4 8 9 14 13
		f 4 8 75 -13 -75
		mu 0 4 10 11 16 15
		f 4 9 76 -14 -76
		mu 0 4 11 12 17 16
		f 4 10 77 -15 -77
		mu 0 4 12 13 18 17
		f 4 11 78 -16 -78
		mu 0 4 13 14 19 18
		f 4 12 80 -17 -80
		mu 0 4 15 16 21 20
		f 4 13 81 -18 -81
		mu 0 4 16 17 22 21
		f 4 14 82 -19 -82
		mu 0 4 17 18 23 22
		f 4 15 83 -20 -83
		mu 0 4 18 19 24 23
		f 4 16 85 -21 -85
		mu 0 4 20 21 26 25
		f 4 17 86 -22 -86
		mu 0 4 21 22 27 26
		f 4 18 87 -23 -87
		mu 0 4 22 23 28 27
		f 4 19 88 -24 -88
		mu 0 4 23 24 29 28
		f 4 20 90 -25 -90
		mu 0 4 25 26 31 30
		f 4 21 91 -26 -91
		mu 0 4 26 27 32 31
		f 4 22 92 -27 -92
		mu 0 4 27 28 33 32
		f 4 23 93 -28 -93
		mu 0 4 28 29 34 33
		f 4 24 95 -29 -95
		mu 0 4 30 31 36 35
		f 4 25 96 -30 -96
		mu 0 4 31 32 37 36
		f 4 26 97 -31 -97
		mu 0 4 32 33 38 37
		f 4 27 98 -32 -98
		mu 0 4 33 34 39 38
		f 4 28 100 -33 -100
		mu 0 4 35 36 41 40
		f 4 29 101 -34 -101
		mu 0 4 36 37 42 41
		f 4 30 102 -35 -102
		mu 0 4 37 38 43 42
		f 4 31 103 -36 -103
		mu 0 4 38 39 44 43
		f 4 32 105 -37 -105
		mu 0 4 40 41 46 45
		f 4 33 106 -38 -106
		mu 0 4 41 42 47 46
		f 4 34 107 -39 -107
		mu 0 4 42 43 48 47
		f 4 35 108 -40 -108
		mu 0 4 43 44 49 48
		f 4 36 110 -41 -110
		mu 0 4 45 46 51 50
		f 4 37 111 -42 -111
		mu 0 4 46 47 52 51
		f 4 38 112 -43 -112
		mu 0 4 47 48 53 52
		f 4 39 113 -44 -113
		mu 0 4 48 49 54 53
		f 4 40 115 -45 -115
		mu 0 4 50 51 56 55
		f 4 41 116 -46 -116
		mu 0 4 51 52 57 56
		f 4 42 117 -47 -117
		mu 0 4 52 53 58 57
		f 4 43 118 -48 -118
		mu 0 4 53 54 59 58
		f 4 44 120 -49 -120
		mu 0 4 55 56 61 60
		f 4 45 121 -50 -121
		mu 0 4 56 57 62 61
		f 4 46 122 -51 -122
		mu 0 4 57 58 63 62
		f 4 47 123 -52 -123
		mu 0 4 58 59 64 63
		f 4 48 125 -53 -125
		mu 0 4 60 61 66 65
		f 4 49 126 -54 -126
		mu 0 4 61 62 67 66
		f 4 50 127 -55 -127
		mu 0 4 62 63 68 67
		f 4 51 128 -56 -128
		mu 0 4 63 64 69 68
		f 4 52 130 -57 -130
		mu 0 4 65 66 71 70
		f 4 53 131 -58 -131
		mu 0 4 66 67 72 71
		f 4 54 132 -59 -132
		mu 0 4 67 68 73 72
		f 4 55 133 -60 -133
		mu 0 4 68 69 74 73
		f 4 56 135 -61 -135
		mu 0 4 70 71 76 75
		f 4 57 136 -62 -136
		mu 0 4 71 72 77 76
		f 4 58 137 -63 -137
		mu 0 4 72 73 78 77
		f 4 59 138 -64 -138
		mu 0 4 73 74 79 78
		f 4 60 140 -1 -140
		mu 0 4 75 76 81 80
		f 4 61 141 -2 -141
		mu 0 4 76 77 82 81
		f 4 62 142 -3 -142
		mu 0 4 77 78 83 82
		f 4 63 143 -4 -143
		mu 0 4 78 79 84 83
		f 4 -129 -124 144 -157
		mu 0 4 86 85 89 90
		f 4 -134 156 145 -158
		mu 0 4 87 86 90 91
		f 4 -139 157 146 -159
		mu 0 4 88 87 91 92
		f 4 -144 158 147 -69
		mu 0 4 4 88 92 9
		f 4 -145 -119 148 -160
		mu 0 4 90 89 93 94
		f 4 -146 159 149 -161
		mu 0 4 91 90 94 95
		f 4 -147 160 150 -162
		mu 0 4 92 91 95 96
		f 4 -148 161 151 -74
		mu 0 4 9 92 96 14
		f 4 -149 -114 152 -163
		mu 0 4 94 93 97 98
		f 4 -150 162 153 -164
		mu 0 4 95 94 98 99
		f 4 -151 163 154 -165
		mu 0 4 96 95 99 100
		f 4 -152 164 155 -79
		mu 0 4 14 96 100 19
		f 4 -153 -109 -104 -166
		mu 0 4 98 97 101 102
		f 4 -154 165 -99 -167
		mu 0 4 99 98 102 103
		f 4 -155 166 -94 -168
		mu 0 4 100 99 103 104
		f 4 -156 167 -89 -84
		mu 0 4 19 100 104 24
		f 4 124 180 -169 119
		mu 0 4 105 106 110 109
		f 4 129 181 -170 -181
		mu 0 4 106 107 111 110
		f 4 134 182 -171 -182
		mu 0 4 107 108 112 111
		f 4 139 64 -172 -183
		mu 0 4 108 0 5 112
		f 4 168 183 -173 114
		mu 0 4 109 110 114 113
		f 4 169 184 -174 -184
		mu 0 4 110 111 115 114
		f 4 170 185 -175 -185
		mu 0 4 111 112 116 115
		f 4 171 69 -176 -186
		mu 0 4 112 5 10 116
		f 4 172 186 -177 109
		mu 0 4 113 114 118 117
		f 4 173 187 -178 -187
		mu 0 4 114 115 119 118
		f 4 174 188 -179 -188
		mu 0 4 115 116 120 119
		f 4 175 74 -180 -189
		mu 0 4 116 10 15 120
		f 4 176 189 99 104
		mu 0 4 117 118 122 121
		f 4 177 190 94 -190
		mu 0 4 118 119 123 122
		f 4 178 191 89 -191
		mu 0 4 119 120 124 123
		f 4 179 79 84 -192
		mu 0 4 120 15 20 124;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "twstHandle";
	rename -uid "D94732BC-4CAE-C85E-C358-85AF1E590A72";
	setAttr ".t" -type "double3" 10 0 -10 ;
	setAttr ".smd" 7;
createNode deformTwist -n "twstHandleShape" -p "twstHandle";
	rename -uid "8D110B52-4397-3BE4-9531-40B1F18D3664";
	setAttr -k off ".v";
	setAttr ".hw" 1.1;
createNode transform -n "squashCube";
	rename -uid "25F95F94-4374-38CD-C9C9-8F94E2A27920";
	setAttr ".rp" -type "double3" 20 0 -10 ;
	setAttr ".sp" -type "double3" 20 0 -10 ;
createNode mesh -n "squashCubeShape" -p "squashCube";
	rename -uid "8588AFD5-4769-B82D-8E62-BF8E6C0CD715";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "squashCubeShapeOrig" -p "squashCube";
	rename -uid "04A8B8D1-43F2-97CE-173E-F7887C4DF83B";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[32:47]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[48:63]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0:15]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[80:95]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[64:79]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[16:31]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 125 ".uvst[0].uvsp[0:124]" -type "float2" 0.33000001 0 0.41333336
		 0 0.4966667 0 0.58000004 0 0.66333336 0 0.33000001 0.0625 0.41333336 0.0625 0.4966667
		 0.0625 0.58000004 0.0625 0.66333336 0.0625 0.33000001 0.125 0.41333336 0.125 0.4966667
		 0.125 0.58000004 0.125 0.66333336 0.125 0.33000001 0.1875 0.41333336 0.1875 0.4966667
		 0.1875 0.58000004 0.1875 0.66333336 0.1875 0.33000001 0.25 0.41333336 0.25 0.4966667
		 0.25 0.58000004 0.25 0.66333336 0.25 0.33000001 0.3125 0.41333336 0.3125 0.4966667
		 0.3125 0.58000004 0.3125 0.66333336 0.3125 0.33000001 0.375 0.41333336 0.375 0.4966667
		 0.375 0.58000004 0.375 0.66333336 0.375 0.33000001 0.4375 0.41333336 0.4375 0.4966667
		 0.4375 0.58000004 0.4375 0.66333336 0.4375 0.33000001 0.5 0.41333336 0.5 0.4966667
		 0.5 0.58000004 0.5 0.66333336 0.5 0.33000001 0.5625 0.41333336 0.5625 0.4966667 0.5625
		 0.58000004 0.5625 0.66333336 0.5625 0.33000001 0.625 0.41333336 0.625 0.4966667 0.625
		 0.58000004 0.625 0.66333336 0.625 0.33000001 0.6875 0.41333336 0.6875 0.4966667 0.6875
		 0.58000004 0.6875 0.66333336 0.6875 0.33000001 0.75 0.41333336 0.75 0.4966667 0.75
		 0.58000004 0.75 0.66333336 0.75 0.33000001 0.8125 0.41333336 0.8125 0.4966667 0.8125
		 0.58000004 0.8125 0.66333336 0.8125 0.33000001 0.875 0.41333336 0.875 0.4966667 0.875
		 0.58000004 0.875 0.66333336 0.875 0.33000001 0.9375 0.41333336 0.9375 0.4966667 0.9375
		 0.58000004 0.9375 0.66333336 0.9375 0.33000001 1 0.41333336 1 0.4966667 1 0.58000004
		 1 0.66333336 1 1 0 0.9375 0 0.875 0 0.8125 0 1 0.0625 0.9375 0.0625 0.875 0.0625
		 0.8125 0.0625 1 0.125 0.9375 0.125 0.875 0.125 0.8125 0.125 1 0.1875 0.9375 0.1875
		 0.875 0.1875 0.8125 0.1875 1 0.25 0.9375 0.25 0.875 0.25 0.8125 0.25 0 0 0.0625 0
		 0.125 0 0.1875 0 0 0.0625 0.0625 0.0625 0.125 0.0625 0.1875 0.0625 0 0.125 0.0625
		 0.125 0.125 0.125 0.1875 0.125 0 0.1875 0.0625 0.1875 0.125 0.1875 0.1875 0.1875
		 0 0.25 0.0625 0.25 0.125 0.25 0.1875 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 98 ".pt[0:97]" -type "float3"  20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10 
		20 0 -10 20 0 -10 20 0 -10 20 0 -10 20 0 -10;
	setAttr -s 98 ".vt[0:97]"  -1 -1 1 -0.5 -1 1 0 -1 1 0.5 -1 1 1 -1 1
		 -1 -0.5 1 -0.5 -0.5 1 0 -0.5 1 0.5 -0.5 1 1 -0.5 1 -1 0 1 -0.5 0 1 0 0 1 0.5 0 1
		 1 0 1 -1 0.5 1 -0.5 0.5 1 0 0.5 1 0.5 0.5 1 1 0.5 1 -1 1 1 -0.5 1 1 0 1 1 0.5 1 1
		 1 1 1 -1 1 0.5 -0.5 1 0.5 0 1 0.5 0.5 1 0.5 1 1 0.5 -1 1 0 -0.5 1 0 0 1 0 0.5 1 0
		 1 1 0 -1 1 -0.5 -0.5 1 -0.5 0 1 -0.5 0.5 1 -0.5 1 1 -0.5 -1 1 -1 -0.5 1 -1 0 1 -1
		 0.5 1 -1 1 1 -1 -1 0.5 -1 -0.5 0.5 -1 0 0.5 -1 0.5 0.5 -1 1 0.5 -1 -1 0 -1 -0.5 0 -1
		 0 0 -1 0.5 0 -1 1 0 -1 -1 -0.5 -1 -0.5 -0.5 -1 0 -0.5 -1 0.5 -0.5 -1 1 -0.5 -1 -1 -1 -1
		 -0.5 -1 -1 0 -1 -1 0.5 -1 -1 1 -1 -1 -1 -1 -0.5 -0.5 -1 -0.5 0 -1 -0.5 0.5 -1 -0.5
		 1 -1 -0.5 -1 -1 0 -0.5 -1 0 0 -1 0 0.5 -1 0 1 -1 0 -1 -1 0.5 -0.5 -1 0.5 0 -1 0.5
		 0.5 -1 0.5 1 -1 0.5 1 -0.5 -0.5 1 -0.5 0 1 -0.5 0.5 1 0 -0.5 1 0 0 1 0 0.5 1 0.5 -0.5
		 1 0.5 0 1 0.5 0.5 -1 -0.5 -0.5 -1 -0.5 0 -1 -0.5 0.5 -1 0 -0.5 -1 0 0 -1 0 0.5 -1 0.5 -0.5
		 -1 0.5 0 -1 0.5 0.5;
	setAttr -s 192 ".ed";
	setAttr ".ed[0:165]"  0 1 0 1 2 0 2 3 0 3 4 0 5 6 1 6 7 1 7 8 1 8 9 1 10 11 1
		 11 12 1 12 13 1 13 14 1 15 16 1 16 17 1 17 18 1 18 19 1 20 21 0 21 22 0 22 23 0 23 24 0
		 25 26 1 26 27 1 27 28 1 28 29 1 30 31 1 31 32 1 32 33 1 33 34 1 35 36 1 36 37 1 37 38 1
		 38 39 1 40 41 0 41 42 0 42 43 0 43 44 0 45 46 1 46 47 1 47 48 1 48 49 1 50 51 1 51 52 1
		 52 53 1 53 54 1 55 56 1 56 57 1 57 58 1 58 59 1 60 61 0 61 62 0 62 63 0 63 64 0 65 66 1
		 66 67 1 67 68 1 68 69 1 70 71 1 71 72 1 72 73 1 73 74 1 75 76 1 76 77 1 77 78 1 78 79 1
		 0 5 0 1 6 1 2 7 1 3 8 1 4 9 0 5 10 0 6 11 1 7 12 1 8 13 1 9 14 0 10 15 0 11 16 1
		 12 17 1 13 18 1 14 19 0 15 20 0 16 21 1 17 22 1 18 23 1 19 24 0 20 25 0 21 26 1 22 27 1
		 23 28 1 24 29 0 25 30 0 26 31 1 27 32 1 28 33 1 29 34 0 30 35 0 31 36 1 32 37 1 33 38 1
		 34 39 0 35 40 0 36 41 1 37 42 1 38 43 1 39 44 0 40 45 0 41 46 1 42 47 1 43 48 1 44 49 0
		 45 50 0 46 51 1 47 52 1 48 53 1 49 54 0 50 55 0 51 56 1 52 57 1 53 58 1 54 59 0 55 60 0
		 56 61 1 57 62 1 58 63 1 59 64 0 60 65 0 61 66 1 62 67 1 63 68 1 64 69 0 65 70 0 66 71 1
		 67 72 1 68 73 1 69 74 0 70 75 0 71 76 1 72 77 1 73 78 1 74 79 0 75 0 0 76 1 1 77 2 1
		 78 3 1 79 4 0 59 80 1 80 81 1 81 82 1 82 9 1 54 83 1 83 84 1 84 85 1 85 14 1 49 86 1
		 86 87 1 87 88 1 88 19 1 69 80 1 74 81 1 79 82 1 80 83 1 81 84 1 82 85 1 83 86 1 84 87 1
		 85 88 1 86 39 1;
	setAttr ".ed[166:191]" 87 34 1 88 29 1 55 89 1 89 90 1 90 91 1 91 5 1 50 92 1
		 92 93 1 93 94 1 94 10 1 45 95 1 95 96 1 96 97 1 97 15 1 65 89 1 70 90 1 75 91 1 89 92 1
		 90 93 1 91 94 1 92 95 1 93 96 1 94 97 1 95 35 1 96 30 1 97 25 1;
	setAttr -s 96 -ch 384 ".fc[0:95]" -type "polyFaces" 
		f 4 0 65 -5 -65
		mu 0 4 0 1 6 5
		f 4 1 66 -6 -66
		mu 0 4 1 2 7 6
		f 4 2 67 -7 -67
		mu 0 4 2 3 8 7
		f 4 3 68 -8 -68
		mu 0 4 3 4 9 8
		f 4 4 70 -9 -70
		mu 0 4 5 6 11 10
		f 4 5 71 -10 -71
		mu 0 4 6 7 12 11
		f 4 6 72 -11 -72
		mu 0 4 7 8 13 12
		f 4 7 73 -12 -73
		mu 0 4 8 9 14 13
		f 4 8 75 -13 -75
		mu 0 4 10 11 16 15
		f 4 9 76 -14 -76
		mu 0 4 11 12 17 16
		f 4 10 77 -15 -77
		mu 0 4 12 13 18 17
		f 4 11 78 -16 -78
		mu 0 4 13 14 19 18
		f 4 12 80 -17 -80
		mu 0 4 15 16 21 20
		f 4 13 81 -18 -81
		mu 0 4 16 17 22 21
		f 4 14 82 -19 -82
		mu 0 4 17 18 23 22
		f 4 15 83 -20 -83
		mu 0 4 18 19 24 23
		f 4 16 85 -21 -85
		mu 0 4 20 21 26 25
		f 4 17 86 -22 -86
		mu 0 4 21 22 27 26
		f 4 18 87 -23 -87
		mu 0 4 22 23 28 27
		f 4 19 88 -24 -88
		mu 0 4 23 24 29 28
		f 4 20 90 -25 -90
		mu 0 4 25 26 31 30
		f 4 21 91 -26 -91
		mu 0 4 26 27 32 31
		f 4 22 92 -27 -92
		mu 0 4 27 28 33 32
		f 4 23 93 -28 -93
		mu 0 4 28 29 34 33
		f 4 24 95 -29 -95
		mu 0 4 30 31 36 35
		f 4 25 96 -30 -96
		mu 0 4 31 32 37 36
		f 4 26 97 -31 -97
		mu 0 4 32 33 38 37
		f 4 27 98 -32 -98
		mu 0 4 33 34 39 38
		f 4 28 100 -33 -100
		mu 0 4 35 36 41 40
		f 4 29 101 -34 -101
		mu 0 4 36 37 42 41
		f 4 30 102 -35 -102
		mu 0 4 37 38 43 42
		f 4 31 103 -36 -103
		mu 0 4 38 39 44 43
		f 4 32 105 -37 -105
		mu 0 4 40 41 46 45
		f 4 33 106 -38 -106
		mu 0 4 41 42 47 46
		f 4 34 107 -39 -107
		mu 0 4 42 43 48 47
		f 4 35 108 -40 -108
		mu 0 4 43 44 49 48
		f 4 36 110 -41 -110
		mu 0 4 45 46 51 50
		f 4 37 111 -42 -111
		mu 0 4 46 47 52 51
		f 4 38 112 -43 -112
		mu 0 4 47 48 53 52
		f 4 39 113 -44 -113
		mu 0 4 48 49 54 53
		f 4 40 115 -45 -115
		mu 0 4 50 51 56 55
		f 4 41 116 -46 -116
		mu 0 4 51 52 57 56
		f 4 42 117 -47 -117
		mu 0 4 52 53 58 57
		f 4 43 118 -48 -118
		mu 0 4 53 54 59 58
		f 4 44 120 -49 -120
		mu 0 4 55 56 61 60
		f 4 45 121 -50 -121
		mu 0 4 56 57 62 61
		f 4 46 122 -51 -122
		mu 0 4 57 58 63 62
		f 4 47 123 -52 -123
		mu 0 4 58 59 64 63
		f 4 48 125 -53 -125
		mu 0 4 60 61 66 65
		f 4 49 126 -54 -126
		mu 0 4 61 62 67 66
		f 4 50 127 -55 -127
		mu 0 4 62 63 68 67
		f 4 51 128 -56 -128
		mu 0 4 63 64 69 68
		f 4 52 130 -57 -130
		mu 0 4 65 66 71 70
		f 4 53 131 -58 -131
		mu 0 4 66 67 72 71
		f 4 54 132 -59 -132
		mu 0 4 67 68 73 72
		f 4 55 133 -60 -133
		mu 0 4 68 69 74 73
		f 4 56 135 -61 -135
		mu 0 4 70 71 76 75
		f 4 57 136 -62 -136
		mu 0 4 71 72 77 76
		f 4 58 137 -63 -137
		mu 0 4 72 73 78 77
		f 4 59 138 -64 -138
		mu 0 4 73 74 79 78
		f 4 60 140 -1 -140
		mu 0 4 75 76 81 80
		f 4 61 141 -2 -141
		mu 0 4 76 77 82 81
		f 4 62 142 -3 -142
		mu 0 4 77 78 83 82
		f 4 63 143 -4 -143
		mu 0 4 78 79 84 83
		f 4 -129 -124 144 -157
		mu 0 4 86 85 89 90
		f 4 -134 156 145 -158
		mu 0 4 87 86 90 91
		f 4 -139 157 146 -159
		mu 0 4 88 87 91 92
		f 4 -144 158 147 -69
		mu 0 4 4 88 92 9
		f 4 -145 -119 148 -160
		mu 0 4 90 89 93 94
		f 4 -146 159 149 -161
		mu 0 4 91 90 94 95
		f 4 -147 160 150 -162
		mu 0 4 92 91 95 96
		f 4 -148 161 151 -74
		mu 0 4 9 92 96 14
		f 4 -149 -114 152 -163
		mu 0 4 94 93 97 98
		f 4 -150 162 153 -164
		mu 0 4 95 94 98 99
		f 4 -151 163 154 -165
		mu 0 4 96 95 99 100
		f 4 -152 164 155 -79
		mu 0 4 14 96 100 19
		f 4 -153 -109 -104 -166
		mu 0 4 98 97 101 102
		f 4 -154 165 -99 -167
		mu 0 4 99 98 102 103
		f 4 -155 166 -94 -168
		mu 0 4 100 99 103 104
		f 4 -156 167 -89 -84
		mu 0 4 19 100 104 24
		f 4 124 180 -169 119
		mu 0 4 105 106 110 109
		f 4 129 181 -170 -181
		mu 0 4 106 107 111 110
		f 4 134 182 -171 -182
		mu 0 4 107 108 112 111
		f 4 139 64 -172 -183
		mu 0 4 108 0 5 112
		f 4 168 183 -173 114
		mu 0 4 109 110 114 113
		f 4 169 184 -174 -184
		mu 0 4 110 111 115 114
		f 4 170 185 -175 -185
		mu 0 4 111 112 116 115
		f 4 171 69 -176 -186
		mu 0 4 112 5 10 116
		f 4 172 186 -177 109
		mu 0 4 113 114 118 117
		f 4 173 187 -178 -187
		mu 0 4 114 115 119 118
		f 4 174 188 -179 -188
		mu 0 4 115 116 120 119
		f 4 175 74 -180 -189
		mu 0 4 116 10 15 120
		f 4 176 189 99 104
		mu 0 4 117 118 122 121
		f 4 177 190 94 -190
		mu 0 4 118 119 123 122
		f 4 178 191 89 -191
		mu 0 4 119 120 124 123
		f 4 179 79 84 -192
		mu 0 4 120 15 20 124;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "sqshHandle";
	rename -uid "CEB6B237-41B3-C19F-05EA-BF9795E3BCB6";
	setAttr ".t" -type "double3" 20 0 -10 ;
	setAttr ".smd" 7;
createNode deformSquash -n "sqshHandleShape" -p "sqshHandle";
	rename -uid "2B60F820-4E26-C2B7-EF04-4F91599701E0";
	setAttr -k off ".v";
	setAttr ".hw" 1.1;
createNode transform -n "sinePlane";
	rename -uid "2F3C52E4-45A9-D0BE-9D22-359B775A4AB0";
	setAttr ".rp" -type "double3" 20 0 0 ;
	setAttr ".sp" -type "double3" 20 0 0 ;
createNode mesh -n "sinePlaneShape" -p "sinePlane";
	rename -uid "4A745DFB-45F8-9A12-0DCE-DFAD789AC92E";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "sinePlaneShapeOrig" -p "sinePlane";
	rename -uid "3E1E55B6-444E-5EFA-BB36-5EAB0F5136AD";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 5 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "e[136:143]";
	setAttr ".gtag[1].gtagnm" -type "string" "front";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 8 "e[0]" "e[2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[12]" "e[14]";
	setAttr ".gtag[2].gtagnm" -type "string" "left";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 8 "e[1]" "e[18]" "e[35]" "e[52]" "e[69]" "e[86]" "e[103]" "e[120]";
	setAttr ".gtag[3].gtagnm" -type "string" "right";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 8 "e[16]" "e[33]" "e[50]" "e[67]" "e[84]" "e[101]" "e[118]" "e[135]";
	setAttr ".gtag[4].gtagnm" -type "string" "rim";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 22 "e[0:2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[12]" "e[14]" "e[16]" "e[18]" "e[33]" "e[35]" "e[50]" "e[52]" "e[67]" "e[69]" "e[84]" "e[86]" "e[101]" "e[103]" "e[118]" "e[120]" "e[135:143]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 81 ".uvst[0].uvsp[0:80]" -type "float2" 0 0 0.125 0 0.25
		 0 0.375 0 0.5 0 0.625 0 0.75 0 0.875 0 1 0 0 0.125 0.125 0.125 0.25 0.125 0.375 0.125
		 0.5 0.125 0.625 0.125 0.75 0.125 0.875 0.125 1 0.125 0 0.25 0.125 0.25 0.25 0.25
		 0.375 0.25 0.5 0.25 0.625 0.25 0.75 0.25 0.875 0.25 1 0.25 0 0.375 0.125 0.375 0.25
		 0.375 0.375 0.375 0.5 0.375 0.625 0.375 0.75 0.375 0.875 0.375 1 0.375 0 0.5 0.125
		 0.5 0.25 0.5 0.375 0.5 0.5 0.5 0.625 0.5 0.75 0.5 0.875 0.5 1 0.5 0 0.625 0.125 0.625
		 0.25 0.625 0.375 0.625 0.5 0.625 0.625 0.625 0.75 0.625 0.875 0.625 1 0.625 0 0.75
		 0.125 0.75 0.25 0.75 0.375 0.75 0.5 0.75 0.625 0.75 0.75 0.75 0.875 0.75 1 0.75 0
		 0.875 0.125 0.875 0.25 0.875 0.375 0.875 0.5 0.875 0.625 0.875 0.75 0.875 0.875 0.875
		 1 0.875 0 1 0.125 1 0.25 1 0.375 1 0.5 1 0.625 1 0.75 1 0.875 1 1 1;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 81 ".pt[0:80]" -type "float3"  20 0 0 20 0 0 20 0 0 20 0 
		0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 
		0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 
		20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 
		0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 
		0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 
		20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 
		0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0 20 0 0;
	setAttr -s 81 ".vt[0:80]"  -2 0 2 -1.5 0 2 -1 0 2 -0.5 0 2 0 0 2 0.5 0 2
		 1 0 2 1.5 0 2 2 0 2 -2 0 1.5 -1.5 0 1.5 -1 0 1.5 -0.5 0 1.5 0 0 1.5 0.5 0 1.5 1 0 1.5
		 1.5 0 1.5 2 0 1.5 -2 0 1 -1.5 0 1 -1 0 1 -0.5 0 1 0 0 1 0.5 0 1 1 0 1 1.5 0 1 2 0 1
		 -2 0 0.5 -1.5 0 0.5 -1 0 0.5 -0.5 0 0.5 0 0 0.5 0.5 0 0.5 1 0 0.5 1.5 0 0.5 2 0 0.5
		 -2 0 0 -1.5 0 0 -1 0 0 -0.5 0 0 0 0 0 0.5 0 0 1 0 0 1.5 0 0 2 0 0 -2 0 -0.5 -1.5 0 -0.5
		 -1 0 -0.5 -0.5 0 -0.5 0 0 -0.5 0.5 0 -0.5 1 0 -0.5 1.5 0 -0.5 2 0 -0.5 -2 0 -1 -1.5 0 -1
		 -1 0 -1 -0.5 0 -1 0 0 -1 0.5 0 -1 1 0 -1 1.5 0 -1 2 0 -1 -2 0 -1.5 -1.5 0 -1.5 -1 0 -1.5
		 -0.5 0 -1.5 0 0 -1.5 0.5 0 -1.5 1 0 -1.5 1.5 0 -1.5 2 0 -1.5 -2 0 -2 -1.5 0 -2 -1 0 -2
		 -0.5 0 -2 0 0 -2 0.5 0 -2 1 0 -2 1.5 0 -2 2 0 -2;
	setAttr -s 144 ".ed[0:143]"  0 1 0 0 9 0 1 2 0 1 10 1 2 3 0 2 11 1 3 4 0
		 3 12 1 4 5 0 4 13 1 5 6 0 5 14 1 6 7 0 6 15 1 7 8 0 7 16 1 8 17 0 9 10 1 9 18 0 10 11 1
		 10 19 1 11 12 1 11 20 1 12 13 1 12 21 1 13 14 1 13 22 1 14 15 1 14 23 1 15 16 1 15 24 1
		 16 17 1 16 25 1 17 26 0 18 19 1 18 27 0 19 20 1 19 28 1 20 21 1 20 29 1 21 22 1 21 30 1
		 22 23 1 22 31 1 23 24 1 23 32 1 24 25 1 24 33 1 25 26 1 25 34 1 26 35 0 27 28 1 27 36 0
		 28 29 1 28 37 1 29 30 1 29 38 1 30 31 1 30 39 1 31 32 1 31 40 1 32 33 1 32 41 1 33 34 1
		 33 42 1 34 35 1 34 43 1 35 44 0 36 37 1 36 45 0 37 38 1 37 46 1 38 39 1 38 47 1 39 40 1
		 39 48 1 40 41 1 40 49 1 41 42 1 41 50 1 42 43 1 42 51 1 43 44 1 43 52 1 44 53 0 45 46 1
		 45 54 0 46 47 1 46 55 1 47 48 1 47 56 1 48 49 1 48 57 1 49 50 1 49 58 1 50 51 1 50 59 1
		 51 52 1 51 60 1 52 53 1 52 61 1 53 62 0 54 55 1 54 63 0 55 56 1 55 64 1 56 57 1 56 65 1
		 57 58 1 57 66 1 58 59 1 58 67 1 59 60 1 59 68 1 60 61 1 60 69 1 61 62 1 61 70 1 62 71 0
		 63 64 1 63 72 0 64 65 1 64 73 1 65 66 1 65 74 1 66 67 1 66 75 1 67 68 1 67 76 1 68 69 1
		 68 77 1 69 70 1 69 78 1 70 71 1 70 79 1 71 80 0 72 73 0 73 74 0 74 75 0 75 76 0 76 77 0
		 77 78 0 78 79 0 79 80 0;
	setAttr -s 64 -ch 256 ".fc[0:63]" -type "polyFaces" 
		f 4 0 3 -18 -2
		mu 0 4 0 1 10 9
		f 4 2 5 -20 -4
		mu 0 4 1 2 11 10
		f 4 4 7 -22 -6
		mu 0 4 2 3 12 11
		f 4 6 9 -24 -8
		mu 0 4 3 4 13 12
		f 4 8 11 -26 -10
		mu 0 4 4 5 14 13
		f 4 10 13 -28 -12
		mu 0 4 5 6 15 14
		f 4 12 15 -30 -14
		mu 0 4 6 7 16 15
		f 4 14 16 -32 -16
		mu 0 4 7 8 17 16
		f 4 17 20 -35 -19
		mu 0 4 9 10 19 18
		f 4 19 22 -37 -21
		mu 0 4 10 11 20 19
		f 4 21 24 -39 -23
		mu 0 4 11 12 21 20
		f 4 23 26 -41 -25
		mu 0 4 12 13 22 21
		f 4 25 28 -43 -27
		mu 0 4 13 14 23 22
		f 4 27 30 -45 -29
		mu 0 4 14 15 24 23
		f 4 29 32 -47 -31
		mu 0 4 15 16 25 24
		f 4 31 33 -49 -33
		mu 0 4 16 17 26 25
		f 4 34 37 -52 -36
		mu 0 4 18 19 28 27
		f 4 36 39 -54 -38
		mu 0 4 19 20 29 28
		f 4 38 41 -56 -40
		mu 0 4 20 21 30 29
		f 4 40 43 -58 -42
		mu 0 4 21 22 31 30
		f 4 42 45 -60 -44
		mu 0 4 22 23 32 31
		f 4 44 47 -62 -46
		mu 0 4 23 24 33 32
		f 4 46 49 -64 -48
		mu 0 4 24 25 34 33
		f 4 48 50 -66 -50
		mu 0 4 25 26 35 34
		f 4 51 54 -69 -53
		mu 0 4 27 28 37 36
		f 4 53 56 -71 -55
		mu 0 4 28 29 38 37
		f 4 55 58 -73 -57
		mu 0 4 29 30 39 38
		f 4 57 60 -75 -59
		mu 0 4 30 31 40 39
		f 4 59 62 -77 -61
		mu 0 4 31 32 41 40
		f 4 61 64 -79 -63
		mu 0 4 32 33 42 41
		f 4 63 66 -81 -65
		mu 0 4 33 34 43 42
		f 4 65 67 -83 -67
		mu 0 4 34 35 44 43
		f 4 68 71 -86 -70
		mu 0 4 36 37 46 45
		f 4 70 73 -88 -72
		mu 0 4 37 38 47 46
		f 4 72 75 -90 -74
		mu 0 4 38 39 48 47
		f 4 74 77 -92 -76
		mu 0 4 39 40 49 48
		f 4 76 79 -94 -78
		mu 0 4 40 41 50 49
		f 4 78 81 -96 -80
		mu 0 4 41 42 51 50
		f 4 80 83 -98 -82
		mu 0 4 42 43 52 51
		f 4 82 84 -100 -84
		mu 0 4 43 44 53 52
		f 4 85 88 -103 -87
		mu 0 4 45 46 55 54
		f 4 87 90 -105 -89
		mu 0 4 46 47 56 55
		f 4 89 92 -107 -91
		mu 0 4 47 48 57 56
		f 4 91 94 -109 -93
		mu 0 4 48 49 58 57
		f 4 93 96 -111 -95
		mu 0 4 49 50 59 58
		f 4 95 98 -113 -97
		mu 0 4 50 51 60 59
		f 4 97 100 -115 -99
		mu 0 4 51 52 61 60
		f 4 99 101 -117 -101
		mu 0 4 52 53 62 61
		f 4 102 105 -120 -104
		mu 0 4 54 55 64 63
		f 4 104 107 -122 -106
		mu 0 4 55 56 65 64
		f 4 106 109 -124 -108
		mu 0 4 56 57 66 65
		f 4 108 111 -126 -110
		mu 0 4 57 58 67 66
		f 4 110 113 -128 -112
		mu 0 4 58 59 68 67
		f 4 112 115 -130 -114
		mu 0 4 59 60 69 68
		f 4 114 117 -132 -116
		mu 0 4 60 61 70 69
		f 4 116 118 -134 -118
		mu 0 4 61 62 71 70
		f 4 119 122 -137 -121
		mu 0 4 63 64 73 72
		f 4 121 124 -138 -123
		mu 0 4 64 65 74 73
		f 4 123 126 -139 -125
		mu 0 4 65 66 75 74
		f 4 125 128 -140 -127
		mu 0 4 66 67 76 75
		f 4 127 130 -141 -129
		mu 0 4 67 68 77 76
		f 4 129 132 -142 -131
		mu 0 4 68 69 78 77
		f 4 131 134 -143 -133
		mu 0 4 69 70 79 78
		f 4 133 135 -144 -135
		mu 0 4 70 71 80 79;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "sneHandle";
	rename -uid "171BE803-4960-8912-71D8-34B54E278BD6";
	setAttr ".t" -type "double3" 20 0 0 ;
	setAttr ".s" -type "double3" 2 2 2 ;
	setAttr ".smd" 7;
createNode deformSine -n "sneHandleShape" -p "sneHandle";
	rename -uid "E956C8A0-4E38-F672-0576-83AAB76D15D7";
	setAttr -k off ".v";
	setAttr ".hw" 2.2;
createNode transform -n "wavePlane";
	rename -uid "4AF0BD08-459F-6B8C-3FBC-66A02C6AC67A";
	setAttr ".rp" -type "double3" 20 0 10 ;
	setAttr ".sp" -type "double3" 20 0 10 ;
createNode mesh -n "wavePlaneShape" -p "wavePlane";
	rename -uid "E0684694-4F1D-0040-E24B-AFA716D3A222";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "wavePlaneShapeOrig" -p "wavePlane";
	rename -uid "7B14CFE4-4F4A-55F6-542E-2384D9B5144B";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 5 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "e[136:143]";
	setAttr ".gtag[1].gtagnm" -type "string" "front";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 8 "e[0]" "e[2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[12]" "e[14]";
	setAttr ".gtag[2].gtagnm" -type "string" "left";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 8 "e[1]" "e[18]" "e[35]" "e[52]" "e[69]" "e[86]" "e[103]" "e[120]";
	setAttr ".gtag[3].gtagnm" -type "string" "right";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 8 "e[16]" "e[33]" "e[50]" "e[67]" "e[84]" "e[101]" "e[118]" "e[135]";
	setAttr ".gtag[4].gtagnm" -type "string" "rim";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 22 "e[0:2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[12]" "e[14]" "e[16]" "e[18]" "e[33]" "e[35]" "e[50]" "e[52]" "e[67]" "e[69]" "e[84]" "e[86]" "e[101]" "e[103]" "e[118]" "e[120]" "e[135:143]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 81 ".uvst[0].uvsp[0:80]" -type "float2" 0 0 0.125 0 0.25
		 0 0.375 0 0.5 0 0.625 0 0.75 0 0.875 0 1 0 0 0.125 0.125 0.125 0.25 0.125 0.375 0.125
		 0.5 0.125 0.625 0.125 0.75 0.125 0.875 0.125 1 0.125 0 0.25 0.125 0.25 0.25 0.25
		 0.375 0.25 0.5 0.25 0.625 0.25 0.75 0.25 0.875 0.25 1 0.25 0 0.375 0.125 0.375 0.25
		 0.375 0.375 0.375 0.5 0.375 0.625 0.375 0.75 0.375 0.875 0.375 1 0.375 0 0.5 0.125
		 0.5 0.25 0.5 0.375 0.5 0.5 0.5 0.625 0.5 0.75 0.5 0.875 0.5 1 0.5 0 0.625 0.125 0.625
		 0.25 0.625 0.375 0.625 0.5 0.625 0.625 0.625 0.75 0.625 0.875 0.625 1 0.625 0 0.75
		 0.125 0.75 0.25 0.75 0.375 0.75 0.5 0.75 0.625 0.75 0.75 0.75 0.875 0.75 1 0.75 0
		 0.875 0.125 0.875 0.25 0.875 0.375 0.875 0.5 0.875 0.625 0.875 0.75 0.875 0.875 0.875
		 1 0.875 0 1 0.125 1 0.25 1 0.375 1 0.5 1 0.625 1 0.75 1 0.875 1 1 1;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 81 ".pt[0:80]" -type "float3"  20 0 10 20 0 10 20 0 10 20 
		0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 
		10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 
		20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 
		0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 
		10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 
		20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 
		0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 
		10 20 0 10 20 0 10 20 0 10 20 0 10 20 0 10;
	setAttr -s 81 ".vt[0:80]"  -2 0 2 -1.5 0 2 -1 0 2 -0.5 0 2 0 0 2 0.5 0 2
		 1 0 2 1.5 0 2 2 0 2 -2 0 1.5 -1.5 0 1.5 -1 0 1.5 -0.5 0 1.5 0 0 1.5 0.5 0 1.5 1 0 1.5
		 1.5 0 1.5 2 0 1.5 -2 0 1 -1.5 0 1 -1 0 1 -0.5 0 1 0 0 1 0.5 0 1 1 0 1 1.5 0 1 2 0 1
		 -2 0 0.5 -1.5 0 0.5 -1 0 0.5 -0.5 0 0.5 0 0 0.5 0.5 0 0.5 1 0 0.5 1.5 0 0.5 2 0 0.5
		 -2 0 0 -1.5 0 0 -1 0 0 -0.5 0 0 0 0 0 0.5 0 0 1 0 0 1.5 0 0 2 0 0 -2 0 -0.5 -1.5 0 -0.5
		 -1 0 -0.5 -0.5 0 -0.5 0 0 -0.5 0.5 0 -0.5 1 0 -0.5 1.5 0 -0.5 2 0 -0.5 -2 0 -1 -1.5 0 -1
		 -1 0 -1 -0.5 0 -1 0 0 -1 0.5 0 -1 1 0 -1 1.5 0 -1 2 0 -1 -2 0 -1.5 -1.5 0 -1.5 -1 0 -1.5
		 -0.5 0 -1.5 0 0 -1.5 0.5 0 -1.5 1 0 -1.5 1.5 0 -1.5 2 0 -1.5 -2 0 -2 -1.5 0 -2 -1 0 -2
		 -0.5 0 -2 0 0 -2 0.5 0 -2 1 0 -2 1.5 0 -2 2 0 -2;
	setAttr -s 144 ".ed[0:143]"  0 1 0 0 9 0 1 2 0 1 10 1 2 3 0 2 11 1 3 4 0
		 3 12 1 4 5 0 4 13 1 5 6 0 5 14 1 6 7 0 6 15 1 7 8 0 7 16 1 8 17 0 9 10 1 9 18 0 10 11 1
		 10 19 1 11 12 1 11 20 1 12 13 1 12 21 1 13 14 1 13 22 1 14 15 1 14 23 1 15 16 1 15 24 1
		 16 17 1 16 25 1 17 26 0 18 19 1 18 27 0 19 20 1 19 28 1 20 21 1 20 29 1 21 22 1 21 30 1
		 22 23 1 22 31 1 23 24 1 23 32 1 24 25 1 24 33 1 25 26 1 25 34 1 26 35 0 27 28 1 27 36 0
		 28 29 1 28 37 1 29 30 1 29 38 1 30 31 1 30 39 1 31 32 1 31 40 1 32 33 1 32 41 1 33 34 1
		 33 42 1 34 35 1 34 43 1 35 44 0 36 37 1 36 45 0 37 38 1 37 46 1 38 39 1 38 47 1 39 40 1
		 39 48 1 40 41 1 40 49 1 41 42 1 41 50 1 42 43 1 42 51 1 43 44 1 43 52 1 44 53 0 45 46 1
		 45 54 0 46 47 1 46 55 1 47 48 1 47 56 1 48 49 1 48 57 1 49 50 1 49 58 1 50 51 1 50 59 1
		 51 52 1 51 60 1 52 53 1 52 61 1 53 62 0 54 55 1 54 63 0 55 56 1 55 64 1 56 57 1 56 65 1
		 57 58 1 57 66 1 58 59 1 58 67 1 59 60 1 59 68 1 60 61 1 60 69 1 61 62 1 61 70 1 62 71 0
		 63 64 1 63 72 0 64 65 1 64 73 1 65 66 1 65 74 1 66 67 1 66 75 1 67 68 1 67 76 1 68 69 1
		 68 77 1 69 70 1 69 78 1 70 71 1 70 79 1 71 80 0 72 73 0 73 74 0 74 75 0 75 76 0 76 77 0
		 77 78 0 78 79 0 79 80 0;
	setAttr -s 64 -ch 256 ".fc[0:63]" -type "polyFaces" 
		f 4 0 3 -18 -2
		mu 0 4 0 1 10 9
		f 4 2 5 -20 -4
		mu 0 4 1 2 11 10
		f 4 4 7 -22 -6
		mu 0 4 2 3 12 11
		f 4 6 9 -24 -8
		mu 0 4 3 4 13 12
		f 4 8 11 -26 -10
		mu 0 4 4 5 14 13
		f 4 10 13 -28 -12
		mu 0 4 5 6 15 14
		f 4 12 15 -30 -14
		mu 0 4 6 7 16 15
		f 4 14 16 -32 -16
		mu 0 4 7 8 17 16
		f 4 17 20 -35 -19
		mu 0 4 9 10 19 18
		f 4 19 22 -37 -21
		mu 0 4 10 11 20 19
		f 4 21 24 -39 -23
		mu 0 4 11 12 21 20
		f 4 23 26 -41 -25
		mu 0 4 12 13 22 21
		f 4 25 28 -43 -27
		mu 0 4 13 14 23 22
		f 4 27 30 -45 -29
		mu 0 4 14 15 24 23
		f 4 29 32 -47 -31
		mu 0 4 15 16 25 24
		f 4 31 33 -49 -33
		mu 0 4 16 17 26 25
		f 4 34 37 -52 -36
		mu 0 4 18 19 28 27
		f 4 36 39 -54 -38
		mu 0 4 19 20 29 28
		f 4 38 41 -56 -40
		mu 0 4 20 21 30 29
		f 4 40 43 -58 -42
		mu 0 4 21 22 31 30
		f 4 42 45 -60 -44
		mu 0 4 22 23 32 31
		f 4 44 47 -62 -46
		mu 0 4 23 24 33 32
		f 4 46 49 -64 -48
		mu 0 4 24 25 34 33
		f 4 48 50 -66 -50
		mu 0 4 25 26 35 34
		f 4 51 54 -69 -53
		mu 0 4 27 28 37 36
		f 4 53 56 -71 -55
		mu 0 4 28 29 38 37
		f 4 55 58 -73 -57
		mu 0 4 29 30 39 38
		f 4 57 60 -75 -59
		mu 0 4 30 31 40 39
		f 4 59 62 -77 -61
		mu 0 4 31 32 41 40
		f 4 61 64 -79 -63
		mu 0 4 32 33 42 41
		f 4 63 66 -81 -65
		mu 0 4 33 34 43 42
		f 4 65 67 -83 -67
		mu 0 4 34 35 44 43
		f 4 68 71 -86 -70
		mu 0 4 36 37 46 45
		f 4 70 73 -88 -72
		mu 0 4 37 38 47 46
		f 4 72 75 -90 -74
		mu 0 4 38 39 48 47
		f 4 74 77 -92 -76
		mu 0 4 39 40 49 48
		f 4 76 79 -94 -78
		mu 0 4 40 41 50 49
		f 4 78 81 -96 -80
		mu 0 4 41 42 51 50
		f 4 80 83 -98 -82
		mu 0 4 42 43 52 51
		f 4 82 84 -100 -84
		mu 0 4 43 44 53 52
		f 4 85 88 -103 -87
		mu 0 4 45 46 55 54
		f 4 87 90 -105 -89
		mu 0 4 46 47 56 55
		f 4 89 92 -107 -91
		mu 0 4 47 48 57 56
		f 4 91 94 -109 -93
		mu 0 4 48 49 58 57
		f 4 93 96 -111 -95
		mu 0 4 49 50 59 58
		f 4 95 98 -113 -97
		mu 0 4 50 51 60 59
		f 4 97 100 -115 -99
		mu 0 4 51 52 61 60
		f 4 99 101 -117 -101
		mu 0 4 52 53 62 61
		f 4 102 105 -120 -104
		mu 0 4 54 55 64 63
		f 4 104 107 -122 -106
		mu 0 4 55 56 65 64
		f 4 106 109 -124 -108
		mu 0 4 56 57 66 65
		f 4 108 111 -126 -110
		mu 0 4 57 58 67 66
		f 4 110 113 -128 -112
		mu 0 4 58 59 68 67
		f 4 112 115 -130 -114
		mu 0 4 59 60 69 68
		f 4 114 117 -132 -116
		mu 0 4 60 61 70 69
		f 4 116 118 -134 -118
		mu 0 4 61 62 71 70
		f 4 119 122 -137 -121
		mu 0 4 63 64 73 72
		f 4 121 124 -138 -123
		mu 0 4 64 65 74 73
		f 4 123 126 -139 -125
		mu 0 4 65 66 75 74
		f 4 125 128 -140 -127
		mu 0 4 66 67 76 75
		f 4 127 130 -141 -129
		mu 0 4 67 68 77 76
		f 4 129 132 -142 -131
		mu 0 4 68 69 78 77
		f 4 131 134 -143 -133
		mu 0 4 69 70 79 78
		f 4 133 135 -144 -135
		mu 0 4 70 71 80 79;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "wveHandle";
	rename -uid "ACB9D97C-4AEF-D672-2761-279D458EC05F";
	setAttr ".t" -type "double3" 20 0 10 ;
	setAttr ".s" -type "double3" 2 2 2 ;
	setAttr ".smd" 7;
createNode deformWave -n "wveHandleShape" -p "wveHandle";
	rename -uid "F16403B4-401F-6737-E9BE-22B1E9566414";
	setAttr -k off ".v";
	setAttr ".hw" 2.2;
createNode transform -n "flareCube";
	rename -uid "FE8AD394-4CC5-F9AB-36FD-DDB9F59061B2";
	setAttr ".rp" -type "double3" 30 0 0 ;
	setAttr ".sp" -type "double3" 30 0 0 ;
createNode mesh -n "flareCubeShape" -p "flareCube";
	rename -uid "C6C2D7B9-488F-F390-1AE3-79BFE430DEC5";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "flareCubeShapeOrig" -p "flareCube";
	rename -uid "EEA714B4-441A-8505-700A-36AE528EF29E";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[32:47]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[48:63]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0:15]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[80:95]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[64:79]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[16:31]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 125 ".uvst[0].uvsp[0:124]" -type "float2" 0.33000001 0 0.41333336
		 0 0.4966667 0 0.58000004 0 0.66333336 0 0.33000001 0.0625 0.41333336 0.0625 0.4966667
		 0.0625 0.58000004 0.0625 0.66333336 0.0625 0.33000001 0.125 0.41333336 0.125 0.4966667
		 0.125 0.58000004 0.125 0.66333336 0.125 0.33000001 0.1875 0.41333336 0.1875 0.4966667
		 0.1875 0.58000004 0.1875 0.66333336 0.1875 0.33000001 0.25 0.41333336 0.25 0.4966667
		 0.25 0.58000004 0.25 0.66333336 0.25 0.33000001 0.3125 0.41333336 0.3125 0.4966667
		 0.3125 0.58000004 0.3125 0.66333336 0.3125 0.33000001 0.375 0.41333336 0.375 0.4966667
		 0.375 0.58000004 0.375 0.66333336 0.375 0.33000001 0.4375 0.41333336 0.4375 0.4966667
		 0.4375 0.58000004 0.4375 0.66333336 0.4375 0.33000001 0.5 0.41333336 0.5 0.4966667
		 0.5 0.58000004 0.5 0.66333336 0.5 0.33000001 0.5625 0.41333336 0.5625 0.4966667 0.5625
		 0.58000004 0.5625 0.66333336 0.5625 0.33000001 0.625 0.41333336 0.625 0.4966667 0.625
		 0.58000004 0.625 0.66333336 0.625 0.33000001 0.6875 0.41333336 0.6875 0.4966667 0.6875
		 0.58000004 0.6875 0.66333336 0.6875 0.33000001 0.75 0.41333336 0.75 0.4966667 0.75
		 0.58000004 0.75 0.66333336 0.75 0.33000001 0.8125 0.41333336 0.8125 0.4966667 0.8125
		 0.58000004 0.8125 0.66333336 0.8125 0.33000001 0.875 0.41333336 0.875 0.4966667 0.875
		 0.58000004 0.875 0.66333336 0.875 0.33000001 0.9375 0.41333336 0.9375 0.4966667 0.9375
		 0.58000004 0.9375 0.66333336 0.9375 0.33000001 1 0.41333336 1 0.4966667 1 0.58000004
		 1 0.66333336 1 1 0 0.9375 0 0.875 0 0.8125 0 1 0.0625 0.9375 0.0625 0.875 0.0625
		 0.8125 0.0625 1 0.125 0.9375 0.125 0.875 0.125 0.8125 0.125 1 0.1875 0.9375 0.1875
		 0.875 0.1875 0.8125 0.1875 1 0.25 0.9375 0.25 0.875 0.25 0.8125 0.25 0 0 0.0625 0
		 0.125 0 0.1875 0 0 0.0625 0.0625 0.0625 0.125 0.0625 0.1875 0.0625 0 0.125 0.0625
		 0.125 0.125 0.125 0.1875 0.125 0 0.1875 0.0625 0.1875 0.125 0.1875 0.1875 0.1875
		 0 0.25 0.0625 0.25 0.125 0.25 0.1875 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 98 ".pt[0:97]" -type "float3"  30 0 0 30 0 0 30 0 0 30 0 
		0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 
		0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 
		30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 
		0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 
		0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 
		30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 
		0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 
		0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 30 0 0 
		30 0 0;
	setAttr -s 98 ".vt[0:97]"  -1 -1 1 -0.5 -1 1 0 -1 1 0.5 -1 1 1 -1 1
		 -1 -0.5 1 -0.5 -0.5 1 0 -0.5 1 0.5 -0.5 1 1 -0.5 1 -1 0 1 -0.5 0 1 0 0 1 0.5 0 1
		 1 0 1 -1 0.5 1 -0.5 0.5 1 0 0.5 1 0.5 0.5 1 1 0.5 1 -1 1 1 -0.5 1 1 0 1 1 0.5 1 1
		 1 1 1 -1 1 0.5 -0.5 1 0.5 0 1 0.5 0.5 1 0.5 1 1 0.5 -1 1 0 -0.5 1 0 0 1 0 0.5 1 0
		 1 1 0 -1 1 -0.5 -0.5 1 -0.5 0 1 -0.5 0.5 1 -0.5 1 1 -0.5 -1 1 -1 -0.5 1 -1 0 1 -1
		 0.5 1 -1 1 1 -1 -1 0.5 -1 -0.5 0.5 -1 0 0.5 -1 0.5 0.5 -1 1 0.5 -1 -1 0 -1 -0.5 0 -1
		 0 0 -1 0.5 0 -1 1 0 -1 -1 -0.5 -1 -0.5 -0.5 -1 0 -0.5 -1 0.5 -0.5 -1 1 -0.5 -1 -1 -1 -1
		 -0.5 -1 -1 0 -1 -1 0.5 -1 -1 1 -1 -1 -1 -1 -0.5 -0.5 -1 -0.5 0 -1 -0.5 0.5 -1 -0.5
		 1 -1 -0.5 -1 -1 0 -0.5 -1 0 0 -1 0 0.5 -1 0 1 -1 0 -1 -1 0.5 -0.5 -1 0.5 0 -1 0.5
		 0.5 -1 0.5 1 -1 0.5 1 -0.5 -0.5 1 -0.5 0 1 -0.5 0.5 1 0 -0.5 1 0 0 1 0 0.5 1 0.5 -0.5
		 1 0.5 0 1 0.5 0.5 -1 -0.5 -0.5 -1 -0.5 0 -1 -0.5 0.5 -1 0 -0.5 -1 0 0 -1 0 0.5 -1 0.5 -0.5
		 -1 0.5 0 -1 0.5 0.5;
	setAttr -s 192 ".ed";
	setAttr ".ed[0:165]"  0 1 0 1 2 0 2 3 0 3 4 0 5 6 1 6 7 1 7 8 1 8 9 1 10 11 1
		 11 12 1 12 13 1 13 14 1 15 16 1 16 17 1 17 18 1 18 19 1 20 21 0 21 22 0 22 23 0 23 24 0
		 25 26 1 26 27 1 27 28 1 28 29 1 30 31 1 31 32 1 32 33 1 33 34 1 35 36 1 36 37 1 37 38 1
		 38 39 1 40 41 0 41 42 0 42 43 0 43 44 0 45 46 1 46 47 1 47 48 1 48 49 1 50 51 1 51 52 1
		 52 53 1 53 54 1 55 56 1 56 57 1 57 58 1 58 59 1 60 61 0 61 62 0 62 63 0 63 64 0 65 66 1
		 66 67 1 67 68 1 68 69 1 70 71 1 71 72 1 72 73 1 73 74 1 75 76 1 76 77 1 77 78 1 78 79 1
		 0 5 0 1 6 1 2 7 1 3 8 1 4 9 0 5 10 0 6 11 1 7 12 1 8 13 1 9 14 0 10 15 0 11 16 1
		 12 17 1 13 18 1 14 19 0 15 20 0 16 21 1 17 22 1 18 23 1 19 24 0 20 25 0 21 26 1 22 27 1
		 23 28 1 24 29 0 25 30 0 26 31 1 27 32 1 28 33 1 29 34 0 30 35 0 31 36 1 32 37 1 33 38 1
		 34 39 0 35 40 0 36 41 1 37 42 1 38 43 1 39 44 0 40 45 0 41 46 1 42 47 1 43 48 1 44 49 0
		 45 50 0 46 51 1 47 52 1 48 53 1 49 54 0 50 55 0 51 56 1 52 57 1 53 58 1 54 59 0 55 60 0
		 56 61 1 57 62 1 58 63 1 59 64 0 60 65 0 61 66 1 62 67 1 63 68 1 64 69 0 65 70 0 66 71 1
		 67 72 1 68 73 1 69 74 0 70 75 0 71 76 1 72 77 1 73 78 1 74 79 0 75 0 0 76 1 1 77 2 1
		 78 3 1 79 4 0 59 80 1 80 81 1 81 82 1 82 9 1 54 83 1 83 84 1 84 85 1 85 14 1 49 86 1
		 86 87 1 87 88 1 88 19 1 69 80 1 74 81 1 79 82 1 80 83 1 81 84 1 82 85 1 83 86 1 84 87 1
		 85 88 1 86 39 1;
	setAttr ".ed[166:191]" 87 34 1 88 29 1 55 89 1 89 90 1 90 91 1 91 5 1 50 92 1
		 92 93 1 93 94 1 94 10 1 45 95 1 95 96 1 96 97 1 97 15 1 65 89 1 70 90 1 75 91 1 89 92 1
		 90 93 1 91 94 1 92 95 1 93 96 1 94 97 1 95 35 1 96 30 1 97 25 1;
	setAttr -s 96 -ch 384 ".fc[0:95]" -type "polyFaces" 
		f 4 0 65 -5 -65
		mu 0 4 0 1 6 5
		f 4 1 66 -6 -66
		mu 0 4 1 2 7 6
		f 4 2 67 -7 -67
		mu 0 4 2 3 8 7
		f 4 3 68 -8 -68
		mu 0 4 3 4 9 8
		f 4 4 70 -9 -70
		mu 0 4 5 6 11 10
		f 4 5 71 -10 -71
		mu 0 4 6 7 12 11
		f 4 6 72 -11 -72
		mu 0 4 7 8 13 12
		f 4 7 73 -12 -73
		mu 0 4 8 9 14 13
		f 4 8 75 -13 -75
		mu 0 4 10 11 16 15
		f 4 9 76 -14 -76
		mu 0 4 11 12 17 16
		f 4 10 77 -15 -77
		mu 0 4 12 13 18 17
		f 4 11 78 -16 -78
		mu 0 4 13 14 19 18
		f 4 12 80 -17 -80
		mu 0 4 15 16 21 20
		f 4 13 81 -18 -81
		mu 0 4 16 17 22 21
		f 4 14 82 -19 -82
		mu 0 4 17 18 23 22
		f 4 15 83 -20 -83
		mu 0 4 18 19 24 23
		f 4 16 85 -21 -85
		mu 0 4 20 21 26 25
		f 4 17 86 -22 -86
		mu 0 4 21 22 27 26
		f 4 18 87 -23 -87
		mu 0 4 22 23 28 27
		f 4 19 88 -24 -88
		mu 0 4 23 24 29 28
		f 4 20 90 -25 -90
		mu 0 4 25 26 31 30
		f 4 21 91 -26 -91
		mu 0 4 26 27 32 31
		f 4 22 92 -27 -92
		mu 0 4 27 28 33 32
		f 4 23 93 -28 -93
		mu 0 4 28 29 34 33
		f 4 24 95 -29 -95
		mu 0 4 30 31 36 35
		f 4 25 96 -30 -96
		mu 0 4 31 32 37 36
		f 4 26 97 -31 -97
		mu 0 4 32 33 38 37
		f 4 27 98 -32 -98
		mu 0 4 33 34 39 38
		f 4 28 100 -33 -100
		mu 0 4 35 36 41 40
		f 4 29 101 -34 -101
		mu 0 4 36 37 42 41
		f 4 30 102 -35 -102
		mu 0 4 37 38 43 42
		f 4 31 103 -36 -103
		mu 0 4 38 39 44 43
		f 4 32 105 -37 -105
		mu 0 4 40 41 46 45
		f 4 33 106 -38 -106
		mu 0 4 41 42 47 46
		f 4 34 107 -39 -107
		mu 0 4 42 43 48 47
		f 4 35 108 -40 -108
		mu 0 4 43 44 49 48
		f 4 36 110 -41 -110
		mu 0 4 45 46 51 50
		f 4 37 111 -42 -111
		mu 0 4 46 47 52 51
		f 4 38 112 -43 -112
		mu 0 4 47 48 53 52
		f 4 39 113 -44 -113
		mu 0 4 48 49 54 53
		f 4 40 115 -45 -115
		mu 0 4 50 51 56 55
		f 4 41 116 -46 -116
		mu 0 4 51 52 57 56
		f 4 42 117 -47 -117
		mu 0 4 52 53 58 57
		f 4 43 118 -48 -118
		mu 0 4 53 54 59 58
		f 4 44 120 -49 -120
		mu 0 4 55 56 61 60
		f 4 45 121 -50 -121
		mu 0 4 56 57 62 61
		f 4 46 122 -51 -122
		mu 0 4 57 58 63 62
		f 4 47 123 -52 -123
		mu 0 4 58 59 64 63
		f 4 48 125 -53 -125
		mu 0 4 60 61 66 65
		f 4 49 126 -54 -126
		mu 0 4 61 62 67 66
		f 4 50 127 -55 -127
		mu 0 4 62 63 68 67
		f 4 51 128 -56 -128
		mu 0 4 63 64 69 68
		f 4 52 130 -57 -130
		mu 0 4 65 66 71 70
		f 4 53 131 -58 -131
		mu 0 4 66 67 72 71
		f 4 54 132 -59 -132
		mu 0 4 67 68 73 72
		f 4 55 133 -60 -133
		mu 0 4 68 69 74 73
		f 4 56 135 -61 -135
		mu 0 4 70 71 76 75
		f 4 57 136 -62 -136
		mu 0 4 71 72 77 76
		f 4 58 137 -63 -137
		mu 0 4 72 73 78 77
		f 4 59 138 -64 -138
		mu 0 4 73 74 79 78
		f 4 60 140 -1 -140
		mu 0 4 75 76 81 80
		f 4 61 141 -2 -141
		mu 0 4 76 77 82 81
		f 4 62 142 -3 -142
		mu 0 4 77 78 83 82
		f 4 63 143 -4 -143
		mu 0 4 78 79 84 83
		f 4 -129 -124 144 -157
		mu 0 4 86 85 89 90
		f 4 -134 156 145 -158
		mu 0 4 87 86 90 91
		f 4 -139 157 146 -159
		mu 0 4 88 87 91 92
		f 4 -144 158 147 -69
		mu 0 4 4 88 92 9
		f 4 -145 -119 148 -160
		mu 0 4 90 89 93 94
		f 4 -146 159 149 -161
		mu 0 4 91 90 94 95
		f 4 -147 160 150 -162
		mu 0 4 92 91 95 96
		f 4 -148 161 151 -74
		mu 0 4 9 92 96 14
		f 4 -149 -114 152 -163
		mu 0 4 94 93 97 98
		f 4 -150 162 153 -164
		mu 0 4 95 94 98 99
		f 4 -151 163 154 -165
		mu 0 4 96 95 99 100
		f 4 -152 164 155 -79
		mu 0 4 14 96 100 19
		f 4 -153 -109 -104 -166
		mu 0 4 98 97 101 102
		f 4 -154 165 -99 -167
		mu 0 4 99 98 102 103
		f 4 -155 166 -94 -168
		mu 0 4 100 99 103 104
		f 4 -156 167 -89 -84
		mu 0 4 19 100 104 24
		f 4 124 180 -169 119
		mu 0 4 105 106 110 109
		f 4 129 181 -170 -181
		mu 0 4 106 107 111 110
		f 4 134 182 -171 -182
		mu 0 4 107 108 112 111
		f 4 139 64 -172 -183
		mu 0 4 108 0 5 112
		f 4 168 183 -173 114
		mu 0 4 109 110 114 113
		f 4 169 184 -174 -184
		mu 0 4 110 111 115 114
		f 4 170 185 -175 -185
		mu 0 4 111 112 116 115
		f 4 171 69 -176 -186
		mu 0 4 112 5 10 116
		f 4 172 186 -177 109
		mu 0 4 113 114 118 117
		f 4 173 187 -178 -187
		mu 0 4 114 115 119 118
		f 4 174 188 -179 -188
		mu 0 4 115 116 120 119
		f 4 175 74 -180 -189
		mu 0 4 116 10 15 120
		f 4 176 189 99 104
		mu 0 4 117 118 122 121
		f 4 177 190 94 -190
		mu 0 4 118 119 123 122
		f 4 178 191 89 -191
		mu 0 4 119 120 124 123
		f 4 179 79 84 -192
		mu 0 4 120 15 20 124;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "flrHandle";
	rename -uid "6C43D0F0-4DEF-2149-708E-B0A3CD640779";
	setAttr ".t" -type "double3" 30 0 0 ;
	setAttr ".smd" 7;
createNode deformFlare -n "flrHandleShape" -p "flrHandle";
	rename -uid "D0E218CE-469D-FDDE-CF8B-E48696713DE7";
	setAttr -k off ".v";
	setAttr ".hw" 1.1;
createNode transform -n "softPlane";
	rename -uid "30E856C5-4A4F-41ED-568B-9A8053DC5D57";
	setAttr ".rp" -type "double3" 10 0 10 ;
	setAttr ".sp" -type "double3" 10 0 10 ;
createNode mesh -n "softPlaneShape" -p "softPlane";
	rename -uid "A1AA45AE-4380-A513-0C28-B695E2161EA9";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "softPlaneShapeOrig" -p "softPlane";
	rename -uid "8F0A1072-4938-7094-C738-BA80A33539DF";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "e[136:143]";
	setAttr ".gtag[1].gtagnm" -type "string" "front";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 8 "e[0]" "e[2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[12]" "e[14]";
	setAttr ".gtag[2].gtagnm" -type "string" "left";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 8 "e[1]" "e[18]" "e[35]" "e[52]" "e[69]" "e[86]" "e[103]" "e[120]";
	setAttr ".gtag[3].gtagnm" -type "string" "right";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 8 "e[16]" "e[33]" "e[50]" "e[67]" "e[84]" "e[101]" "e[118]" "e[135]";
	setAttr ".gtag[4].gtagnm" -type "string" "rim";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 22 "e[0:2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[12]" "e[14]" "e[16]" "e[18]" "e[33]" "e[35]" "e[50]" "e[52]" "e[67]" "e[69]" "e[84]" "e[86]" "e[101]" "e[103]" "e[118]" "e[120]" "e[135:143]";
	setAttr ".gtag[5].gtagnm" -type "string" "soft";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "vtx[40]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 81 ".uvst[0].uvsp[0:80]" -type "float2" 0 0 0.125 0 0.25
		 0 0.375 0 0.5 0 0.625 0 0.75 0 0.875 0 1 0 0 0.125 0.125 0.125 0.25 0.125 0.375 0.125
		 0.5 0.125 0.625 0.125 0.75 0.125 0.875 0.125 1 0.125 0 0.25 0.125 0.25 0.25 0.25
		 0.375 0.25 0.5 0.25 0.625 0.25 0.75 0.25 0.875 0.25 1 0.25 0 0.375 0.125 0.375 0.25
		 0.375 0.375 0.375 0.5 0.375 0.625 0.375 0.75 0.375 0.875 0.375 1 0.375 0 0.5 0.125
		 0.5 0.25 0.5 0.375 0.5 0.5 0.5 0.625 0.5 0.75 0.5 0.875 0.5 1 0.5 0 0.625 0.125 0.625
		 0.25 0.625 0.375 0.625 0.5 0.625 0.625 0.625 0.75 0.625 0.875 0.625 1 0.625 0 0.75
		 0.125 0.75 0.25 0.75 0.375 0.75 0.5 0.75 0.625 0.75 0.75 0.75 0.875 0.75 1 0.75 0
		 0.875 0.125 0.875 0.25 0.875 0.375 0.875 0.5 0.875 0.625 0.875 0.75 0.875 0.875 0.875
		 1 0.875 0 1 0.125 1 0.25 1 0.375 1 0.5 1 0.625 1 0.75 1 0.875 1 1 1;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 81 ".pt[0:80]" -type "float3"  10 0 10 10 0 10 10 0 10 10 
		0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 
		10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 
		10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 
		0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 
		10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 
		10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 
		0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 
		10 10 0 10 10 0 10 10 0 10 10 0 10 10 0 10;
	setAttr -s 81 ".vt[0:80]"  -2 0 2 -1.5 0 2 -1 0 2 -0.5 0 2 0 0 2 0.5 0 2
		 1 0 2 1.5 0 2 2 0 2 -2 0 1.5 -1.5 0 1.5 -1 0 1.5 -0.5 0 1.5 0 0 1.5 0.5 0 1.5 1 0 1.5
		 1.5 0 1.5 2 0 1.5 -2 0 1 -1.5 0 1 -1 0 1 -0.5 0 1 0 0 1 0.5 0 1 1 0 1 1.5 0 1 2 0 1
		 -2 0 0.5 -1.5 0 0.5 -1 0 0.5 -0.5 0 0.5 0 0 0.5 0.5 0 0.5 1 0 0.5 1.5 0 0.5 2 0 0.5
		 -2 0 0 -1.5 0 0 -1 0 0 -0.5 0 0 0 0 0 0.5 0 0 1 0 0 1.5 0 0 2 0 0 -2 0 -0.5 -1.5 0 -0.5
		 -1 0 -0.5 -0.5 0 -0.5 0 0 -0.5 0.5 0 -0.5 1 0 -0.5 1.5 0 -0.5 2 0 -0.5 -2 0 -1 -1.5 0 -1
		 -1 0 -1 -0.5 0 -1 0 0 -1 0.5 0 -1 1 0 -1 1.5 0 -1 2 0 -1 -2 0 -1.5 -1.5 0 -1.5 -1 0 -1.5
		 -0.5 0 -1.5 0 0 -1.5 0.5 0 -1.5 1 0 -1.5 1.5 0 -1.5 2 0 -1.5 -2 0 -2 -1.5 0 -2 -1 0 -2
		 -0.5 0 -2 0 0 -2 0.5 0 -2 1 0 -2 1.5 0 -2 2 0 -2;
	setAttr -s 144 ".ed[0:143]"  0 1 0 0 9 0 1 2 0 1 10 1 2 3 0 2 11 1 3 4 0
		 3 12 1 4 5 0 4 13 1 5 6 0 5 14 1 6 7 0 6 15 1 7 8 0 7 16 1 8 17 0 9 10 1 9 18 0 10 11 1
		 10 19 1 11 12 1 11 20 1 12 13 1 12 21 1 13 14 1 13 22 1 14 15 1 14 23 1 15 16 1 15 24 1
		 16 17 1 16 25 1 17 26 0 18 19 1 18 27 0 19 20 1 19 28 1 20 21 1 20 29 1 21 22 1 21 30 1
		 22 23 1 22 31 1 23 24 1 23 32 1 24 25 1 24 33 1 25 26 1 25 34 1 26 35 0 27 28 1 27 36 0
		 28 29 1 28 37 1 29 30 1 29 38 1 30 31 1 30 39 1 31 32 1 31 40 1 32 33 1 32 41 1 33 34 1
		 33 42 1 34 35 1 34 43 1 35 44 0 36 37 1 36 45 0 37 38 1 37 46 1 38 39 1 38 47 1 39 40 1
		 39 48 1 40 41 1 40 49 1 41 42 1 41 50 1 42 43 1 42 51 1 43 44 1 43 52 1 44 53 0 45 46 1
		 45 54 0 46 47 1 46 55 1 47 48 1 47 56 1 48 49 1 48 57 1 49 50 1 49 58 1 50 51 1 50 59 1
		 51 52 1 51 60 1 52 53 1 52 61 1 53 62 0 54 55 1 54 63 0 55 56 1 55 64 1 56 57 1 56 65 1
		 57 58 1 57 66 1 58 59 1 58 67 1 59 60 1 59 68 1 60 61 1 60 69 1 61 62 1 61 70 1 62 71 0
		 63 64 1 63 72 0 64 65 1 64 73 1 65 66 1 65 74 1 66 67 1 66 75 1 67 68 1 67 76 1 68 69 1
		 68 77 1 69 70 1 69 78 1 70 71 1 70 79 1 71 80 0 72 73 0 73 74 0 74 75 0 75 76 0 76 77 0
		 77 78 0 78 79 0 79 80 0;
	setAttr -s 64 -ch 256 ".fc[0:63]" -type "polyFaces" 
		f 4 0 3 -18 -2
		mu 0 4 0 1 10 9
		f 4 2 5 -20 -4
		mu 0 4 1 2 11 10
		f 4 4 7 -22 -6
		mu 0 4 2 3 12 11
		f 4 6 9 -24 -8
		mu 0 4 3 4 13 12
		f 4 8 11 -26 -10
		mu 0 4 4 5 14 13
		f 4 10 13 -28 -12
		mu 0 4 5 6 15 14
		f 4 12 15 -30 -14
		mu 0 4 6 7 16 15
		f 4 14 16 -32 -16
		mu 0 4 7 8 17 16
		f 4 17 20 -35 -19
		mu 0 4 9 10 19 18
		f 4 19 22 -37 -21
		mu 0 4 10 11 20 19
		f 4 21 24 -39 -23
		mu 0 4 11 12 21 20
		f 4 23 26 -41 -25
		mu 0 4 12 13 22 21
		f 4 25 28 -43 -27
		mu 0 4 13 14 23 22
		f 4 27 30 -45 -29
		mu 0 4 14 15 24 23
		f 4 29 32 -47 -31
		mu 0 4 15 16 25 24
		f 4 31 33 -49 -33
		mu 0 4 16 17 26 25
		f 4 34 37 -52 -36
		mu 0 4 18 19 28 27
		f 4 36 39 -54 -38
		mu 0 4 19 20 29 28
		f 4 38 41 -56 -40
		mu 0 4 20 21 30 29
		f 4 40 43 -58 -42
		mu 0 4 21 22 31 30
		f 4 42 45 -60 -44
		mu 0 4 22 23 32 31
		f 4 44 47 -62 -46
		mu 0 4 23 24 33 32
		f 4 46 49 -64 -48
		mu 0 4 24 25 34 33
		f 4 48 50 -66 -50
		mu 0 4 25 26 35 34
		f 4 51 54 -69 -53
		mu 0 4 27 28 37 36
		f 4 53 56 -71 -55
		mu 0 4 28 29 38 37
		f 4 55 58 -73 -57
		mu 0 4 29 30 39 38
		f 4 57 60 -75 -59
		mu 0 4 30 31 40 39
		f 4 59 62 -77 -61
		mu 0 4 31 32 41 40
		f 4 61 64 -79 -63
		mu 0 4 32 33 42 41
		f 4 63 66 -81 -65
		mu 0 4 33 34 43 42
		f 4 65 67 -83 -67
		mu 0 4 34 35 44 43
		f 4 68 71 -86 -70
		mu 0 4 36 37 46 45
		f 4 70 73 -88 -72
		mu 0 4 37 38 47 46
		f 4 72 75 -90 -74
		mu 0 4 38 39 48 47
		f 4 74 77 -92 -76
		mu 0 4 39 40 49 48
		f 4 76 79 -94 -78
		mu 0 4 40 41 50 49
		f 4 78 81 -96 -80
		mu 0 4 41 42 51 50
		f 4 80 83 -98 -82
		mu 0 4 42 43 52 51
		f 4 82 84 -100 -84
		mu 0 4 43 44 53 52
		f 4 85 88 -103 -87
		mu 0 4 45 46 55 54
		f 4 87 90 -105 -89
		mu 0 4 46 47 56 55
		f 4 89 92 -107 -91
		mu 0 4 47 48 57 56
		f 4 91 94 -109 -93
		mu 0 4 48 49 58 57
		f 4 93 96 -111 -95
		mu 0 4 49 50 59 58
		f 4 95 98 -113 -97
		mu 0 4 50 51 60 59
		f 4 97 100 -115 -99
		mu 0 4 51 52 61 60
		f 4 99 101 -117 -101
		mu 0 4 52 53 62 61
		f 4 102 105 -120 -104
		mu 0 4 54 55 64 63
		f 4 104 107 -122 -106
		mu 0 4 55 56 65 64
		f 4 106 109 -124 -108
		mu 0 4 56 57 66 65
		f 4 108 111 -126 -110
		mu 0 4 57 58 67 66
		f 4 110 113 -128 -112
		mu 0 4 58 59 68 67
		f 4 112 115 -130 -114
		mu 0 4 59 60 69 68
		f 4 114 117 -132 -116
		mu 0 4 60 61 70 69
		f 4 116 118 -134 -118
		mu 0 4 61 62 71 70
		f 4 119 122 -137 -121
		mu 0 4 63 64 73 72
		f 4 121 124 -138 -123
		mu 0 4 64 65 74 73
		f 4 123 126 -139 -125
		mu 0 4 65 66 75 74
		f 4 125 128 -140 -127
		mu 0 4 66 67 76 75
		f 4 127 130 -141 -129
		mu 0 4 67 68 77 76
		f 4 129 132 -142 -131
		mu 0 4 68 69 78 77
		f 4 131 134 -143 -133
		mu 0 4 69 70 79 78
		f 4 133 135 -144 -135
		mu 0 4 70 71 80 79;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "softHandle";
	rename -uid "64355DC4-4668-2622-112D-8BA03C4076D5";
	setAttr ".rp" -type "double3" 10 0 10 ;
	setAttr ".sp" -type "double3" 10 0 10 ;
createNode softModHandle -n "softHandleShape" -p "softHandle";
	rename -uid "39520289-4486-FC20-EC37-E8BE3144B348";
	setAttr ".ihi" 0;
	setAttr -k off ".v";
	setAttr ".or" -type "double3" 10 0 10 ;
createNode lightLinker -s -n "lightLinker1";
	rename -uid "5D47E8C2-4B72-3389-B8A9-5BA4AF0C0EB7";
	setAttr -s 2 ".lnk";
	setAttr -s 2 ".slnk";
createNode displayLayerManager -n "layerManager";
	rename -uid "4FDCE680-4F35-A741-007D-7E8A2DC2A12D";
createNode displayLayer -n "defaultLayer";
	rename -uid "629D87B8-4119-1F8B-2DC7-419EE1B1E753";
	setAttr ".ufem" -type "stringArray" 0  ;
createNode renderLayerManager -n "renderLayerManager";
	rename -uid "582A3612-4AAD-93ED-5AB2-628FB1A5DE79";
createNode renderLayer -n "defaultRenderLayer";
	rename -uid "3EF928CD-440E-078C-A9CB-B5BC5C2E1911";
	setAttr ".g" yes;
createNode cluster -n "cluHandleCluster";
	rename -uid "94EC4C0C-4269-461D-FB13-72ACFE0AC90C";
	setAttr ".ip[0].gtg" -type "string" "clu";
	setAttr -s 5 ".wl[0].w[5:9]"  0.5 0.5 0.5 0.5 0.5;
	setAttr ".gm[0]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
createNode cluster -n "cluB";
	rename -uid "69198F83-4A34-B1D7-B6C2-3087BA4DE1BC";
	setAttr ".gm[0]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
createNode objectSet -n "cluBSet";
	rename -uid "39D47997-4ED6-EA8A-FD6B-BAA28F0B65AC";
	setAttr ".ihi" 0;
	setAttr ".vo" yes;
createNode groupId -n "cluBGroupId";
	rename -uid "9247B34E-4C5C-1F39-2B21-6B8460BA6482";
	setAttr ".ihi" 0;
createNode groupParts -n "cluBGroupParts";
	rename -uid "CBFDE74F-4241-AE9D-375D-8CA1101226FF";
	setAttr ".ihi" 0;
	setAttr ".ic" -type "componentList" 1 "vtx[0:4]";
createNode ffd -n "lat";
	rename -uid "BFB17148-4988-3CC7-C99A-ABB3C2F68B18";
createNode nonLinear -n "bnd";
	rename -uid "6490F86C-4BC6-18DD-218F-FBA9A4E1A9F9";
	addAttr -is true -ci true -k true -sn "cur" -ln "curvature" -smn -3.14159 -smx 
		3.14159 -at "doubleAngle";
	addAttr -is true -ci true -k true -sn "lb" -ln "lowBound" -dv -1 -max 0 -smn -10 
		-smx 0 -at "double";
	addAttr -is true -ci true -k true -sn "hb" -ln "highBound" -dv 1 -min 0 -smn 0 -smx 
		10 -at "double";
	setAttr -k on ".cur" 59.999999999999993;
	setAttr -k on ".lb";
	setAttr -k on ".hb";
createNode nonLinear -n "twst";
	rename -uid "0FC32335-4D9F-1182-406B-B0AD750CCEE2";
	addAttr -is true -ci true -k true -sn "sa" -ln "startAngle" -smn -15 -smx 15 -at "doubleAngle";
	addAttr -is true -ci true -k true -sn "ea" -ln "endAngle" -smn -15 -smx 15 -at "doubleAngle";
	addAttr -is true -ci true -k true -sn "lb" -ln "lowBound" -dv -1 -max 0 -smn -10 
		-smx 0 -at "double";
	addAttr -is true -ci true -k true -sn "hb" -ln "highBound" -dv 1 -min 0 -smn 0 -smx 
		10 -at "double";
	setAttr -k on ".sa" -29.999999999999996;
	setAttr -k on ".ea" 90;
	setAttr -k on ".lb";
	setAttr -k on ".hb";
createNode nonLinear -n "sqsh";
	rename -uid "2B79C553-4433-F9A8-3976-2C96522B4E5E";
	addAttr -is true -ci true -k true -sn "fac" -ln "factor" -smn -10 -smx 10 -at "double";
	addAttr -is true -ci true -k true -sn "exp" -ln "expand" -dv 1 -min 0 -smn 0 -smx 
		10 -at "double";
	addAttr -is true -ci true -k true -sn "mp" -ln "maxExpandPos" -dv 0.5 -min 0.01 
		-max 0.99 -at "double";
	addAttr -is true -ci true -k true -sn "ss" -ln "startSmoothness" -min 0 -max 1 -at "double";
	addAttr -is true -ci true -k true -sn "es" -ln "endSmoothness" -min 0 -max 1 -at "double";
	addAttr -is true -ci true -k true -sn "lb" -ln "lowBound" -dv -1 -max 0 -smn -10 
		-smx 0 -at "double";
	addAttr -is true -ci true -k true -sn "hb" -ln "highBound" -dv 1 -min 0 -smn 0 -smx 
		10 -at "double";
	setAttr -k on ".fac" -0.4;
	setAttr -k on ".exp";
	setAttr -k on ".mp";
	setAttr -k on ".ss";
	setAttr -k on ".es";
	setAttr -k on ".lb";
	setAttr -k on ".hb";
createNode nonLinear -n "sne";
	rename -uid "7883E37F-494B-E2FF-2954-4395599CF3D5";
	addAttr -is true -ci true -k true -sn "amp" -ln "amplitude" -smn -5 -smx 5 -at "double";
	addAttr -is true -ci true -k true -sn "wav" -ln "wavelength" -dv 2 -min 0.001 -smn 
		0.1 -smx 10 -at "double";
	addAttr -is true -ci true -k true -sn "off" -ln "offset" -smn -10 -smx 10 -at "double";
	addAttr -is true -ci true -k true -sn "dr" -ln "dropoff" -min -1 -max 1 -at "double";
	addAttr -is true -ci true -k true -sn "lb" -ln "lowBound" -dv -1 -max 0 -smn -10 
		-smx 0 -at "double";
	addAttr -is true -ci true -k true -sn "hb" -ln "highBound" -dv 1 -min 0 -smn 0 -smx 
		10 -at "double";
	setAttr -k on ".amp" 0.3;
	setAttr -k on ".wav" 1.5;
	setAttr -k on ".off";
	setAttr -k on ".dr";
	setAttr -k on ".lb";
	setAttr -k on ".hb";
createNode nonLinear -n "wve";
	rename -uid "E60CF7D8-46FE-4704-68C0-28800D48A110";
	addAttr -is true -ci true -k true -sn "amp" -ln "amplitude" -smn -5 -smx 5 -at "double";
	addAttr -is true -ci true -k true -sn "wav" -ln "wavelength" -dv 1 -min 0.1 -smn 
		0.1 -smx 10 -at "double";
	addAttr -is true -ci true -k true -sn "off" -ln "offset" -smn -10 -smx 10 -at "double";
	addAttr -is true -ci true -k true -sn "dr" -ln "dropoff" -min -1 -max 1 -at "double";
	addAttr -is true -ci true -k true -sn "dp" -ln "dropoffPosition" -min 0 -max 1 -at "double";
	addAttr -is true -ci true -k true -sn "mnr" -ln "minRadius" -min 0 -smn 0 -smx 10 
		-at "double";
	addAttr -is true -ci true -k true -sn "mxr" -ln "maxRadius" -dv 1 -min 0 -smn 0 
		-smx 10 -at "double";
	setAttr -k on ".amp" 0.2;
	setAttr -k on ".wav" 0.8;
	setAttr -k on ".off";
	setAttr -k on ".dr";
	setAttr -k on ".dp";
	setAttr -k on ".mnr";
	setAttr -k on ".mxr";
createNode nonLinear -n "flr";
	rename -uid "A34EE7E1-41F5-26AF-6477-798366748469";
	addAttr -is true -ci true -k true -sn "sfx" -ln "startFlareX" -dv 1 -smn 0 -smx 
		10 -at "double";
	addAttr -is true -ci true -k true -sn "sfz" -ln "startFlareZ" -dv 1 -smn 0 -smx 
		10 -at "double";
	addAttr -is true -ci true -k true -sn "efx" -ln "endFlareX" -dv 1 -smn 0 -smx 10 
		-at "double";
	addAttr -is true -ci true -k true -sn "efz" -ln "endFlareZ" -dv 1 -smn 0 -smx 10 
		-at "double";
	addAttr -is true -ci true -k true -sn "crv" -ln "curve" -smn -3 -smx 3 -at "double";
	addAttr -is true -ci true -k true -sn "lb" -ln "lowBound" -dv -1 -max 0 -smn -10 
		-smx 0 -at "double";
	addAttr -is true -ci true -k true -sn "hb" -ln "highBound" -dv 1 -min 0 -smn 0 -smx 
		10 -at "double";
	setAttr -k on ".sfx" 1.5;
	setAttr -k on ".sfz" 1.5;
	setAttr -k on ".efx" 0.5;
	setAttr -k on ".efz" 0.5;
	setAttr -k on ".crv";
	setAttr -k on ".lb";
	setAttr -k on ".hb";
createNode softMod -n "softHandleSoftMod";
	rename -uid "C9A1E0DC-4207-A69C-68CE-618F8F0F070A";
	setAttr ".ip[0].gtg" -type "string" "soft";
	setAttr ".gm[0]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
	setAttr -s 2 ".fc[0:1]"  0 1 2 1 0 2;
	setAttr ".fr" 1.5;
createNode script -n "uiConfigurationScriptNode";
	rename -uid "033CFF94-4249-5C34-94E9-55BA8CC2FD57";
	setAttr ".b" -type "string" "// Maya Mel UI Configuration File.\n// No UI generated in batch mode.\n";
	setAttr ".st" 3;
createNode script -n "sceneConfigurationScriptNode";
	rename -uid "9153E91C-4C7A-76AA-9394-E296A6BAA293";
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
	setAttr -s 10 ".dsm";
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
connectAttr "cluHandleCluster.og[0]" "clusterPlaneShape.i";
connectAttr "cluBGroupId.id" "clusterPlaneBShape.iog.og[0].gid";
connectAttr "cluBSet.mwc" "clusterPlaneBShape.iog.og[0].gco";
connectAttr "cluB.og[0]" "clusterPlaneBShape.i";
connectAttr "lat.og[0]" "latCubeShape.i";
connectAttr "bnd.og[0]" "bendCubeShape.i";
connectAttr "bnd.msg" "bndHandle.sml";
connectAttr "bnd.cur" "bndHandleShape.cur";
connectAttr "bnd.lb" "bndHandleShape.lb";
connectAttr "bnd.hb" "bndHandleShape.hb";
connectAttr "twst.og[0]" "twistCubeShape.i";
connectAttr "twst.msg" "twstHandle.sml";
connectAttr "twst.sa" "twstHandleShape.sa";
connectAttr "twst.ea" "twstHandleShape.ea";
connectAttr "twst.lb" "twstHandleShape.lb";
connectAttr "twst.hb" "twstHandleShape.hb";
connectAttr "sqsh.og[0]" "squashCubeShape.i";
connectAttr "sqsh.msg" "sqshHandle.sml";
connectAttr "sqsh.fac" "sqshHandleShape.fac";
connectAttr "sqsh.exp" "sqshHandleShape.exp";
connectAttr "sqsh.mp" "sqshHandleShape.mp";
connectAttr "sqsh.ss" "sqshHandleShape.ss";
connectAttr "sqsh.es" "sqshHandleShape.es";
connectAttr "sqsh.lb" "sqshHandleShape.lb";
connectAttr "sqsh.hb" "sqshHandleShape.hb";
connectAttr "sne.og[0]" "sinePlaneShape.i";
connectAttr "sne.msg" "sneHandle.sml";
connectAttr "sne.amp" "sneHandleShape.amp";
connectAttr "sne.wav" "sneHandleShape.wav";
connectAttr "sne.off" "sneHandleShape.off";
connectAttr "sne.dr" "sneHandleShape.dr";
connectAttr "sne.lb" "sneHandleShape.lb";
connectAttr "sne.hb" "sneHandleShape.hb";
connectAttr "wve.og[0]" "wavePlaneShape.i";
connectAttr "wve.msg" "wveHandle.sml";
connectAttr "wve.amp" "wveHandleShape.amp";
connectAttr "wve.wav" "wveHandleShape.wav";
connectAttr "wve.off" "wveHandleShape.off";
connectAttr "wve.dr" "wveHandleShape.dr";
connectAttr "wve.dp" "wveHandleShape.dp";
connectAttr "wve.mnr" "wveHandleShape.mnr";
connectAttr "wve.mxr" "wveHandleShape.mxr";
connectAttr "flr.og[0]" "flareCubeShape.i";
connectAttr "flr.msg" "flrHandle.sml";
connectAttr "flr.sfx" "flrHandleShape.sfx";
connectAttr "flr.sfz" "flrHandleShape.sfz";
connectAttr "flr.efx" "flrHandleShape.efx";
connectAttr "flr.efz" "flrHandleShape.efz";
connectAttr "flr.crv" "flrHandleShape.crv";
connectAttr "flr.lb" "flrHandleShape.lb";
connectAttr "flr.hb" "flrHandleShape.hb";
connectAttr "softHandleSoftMod.og[0]" "softPlaneShape.i";
relationship "link" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
connectAttr "layerManager.dli[0]" "defaultLayer.id";
connectAttr "renderLayerManager.rlmi[0]" "defaultRenderLayer.rlid";
connectAttr "clusterPlaneShapeOrig.w" "cluHandleCluster.ip[0].ig";
connectAttr "clusterPlaneShapeOrig.o" "cluHandleCluster.orggeom[0]";
connectAttr "cluHandle.wm" "cluHandleCluster.ma";
connectAttr "cluHandleShape.x" "cluHandleCluster.x";
connectAttr "cluBGroupParts.og" "cluB.ip[0].ig";
connectAttr "cluBGroupId.id" "cluB.ip[0].gi";
connectAttr "clusterPlaneBShapeOrig.o" "cluB.orggeom[0]";
connectAttr "cluBHandle.wm" "cluB.ma";
connectAttr "cluBHandleShape.x" "cluB.x";
connectAttr "cluBGroupId.msg" "cluBSet.gn" -na;
connectAttr "clusterPlaneBShape.iog.og[0]" "cluBSet.dsm" -na;
connectAttr "cluB.msg" "cluBSet.ub[0]";
connectAttr "clusterPlaneBShapeOrig.w" "cluBGroupParts.ig";
connectAttr "cluBGroupId.id" "cluBGroupParts.gi";
connectAttr "latCubeShapeOrig.w" "lat.ip[0].ig";
connectAttr "latCubeShapeOrig.o" "lat.orggeom[0]";
connectAttr "latLatticeShape.wm" "lat.dlm";
connectAttr "latLatticeShape.lo" "lat.dlp";
connectAttr "latBaseShape.wm" "lat.blm";
connectAttr "bendCubeShapeOrig.w" "bnd.ip[0].ig";
connectAttr "bendCubeShapeOrig.o" "bnd.orggeom[0]";
connectAttr "bndHandleShape.dd" "bnd.dd";
connectAttr "bndHandle.wm" "bnd.ma";
connectAttr "twistCubeShapeOrig.w" "twst.ip[0].ig";
connectAttr "twistCubeShapeOrig.o" "twst.orggeom[0]";
connectAttr "twstHandleShape.dd" "twst.dd";
connectAttr "twstHandle.wm" "twst.ma";
connectAttr "squashCubeShapeOrig.w" "sqsh.ip[0].ig";
connectAttr "squashCubeShapeOrig.o" "sqsh.orggeom[0]";
connectAttr "sqshHandleShape.dd" "sqsh.dd";
connectAttr "sqshHandle.wm" "sqsh.ma";
connectAttr "sinePlaneShapeOrig.w" "sne.ip[0].ig";
connectAttr "sinePlaneShapeOrig.o" "sne.orggeom[0]";
connectAttr "sneHandleShape.dd" "sne.dd";
connectAttr "sneHandle.wm" "sne.ma";
connectAttr "wavePlaneShapeOrig.w" "wve.ip[0].ig";
connectAttr "wavePlaneShapeOrig.o" "wve.orggeom[0]";
connectAttr "wveHandleShape.dd" "wve.dd";
connectAttr "wveHandle.wm" "wve.ma";
connectAttr "flareCubeShapeOrig.w" "flr.ip[0].ig";
connectAttr "flareCubeShapeOrig.o" "flr.orggeom[0]";
connectAttr "flrHandleShape.dd" "flr.dd";
connectAttr "flrHandle.wm" "flr.ma";
connectAttr "softPlaneShapeOrig.w" "softHandleSoftMod.ip[0].ig";
connectAttr "softPlaneShapeOrig.o" "softHandleSoftMod.orggeom[0]";
connectAttr "softHandle.wm" "softHandleSoftMod.ma";
connectAttr "softHandleShape.x" "softHandleSoftMod.x";
connectAttr "defaultRenderLayer.msg" ":defaultRenderingList1.r" -na;
connectAttr "clusterPlaneShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "clusterPlaneBShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "latCubeShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "bendCubeShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "twistCubeShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "squashCubeShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "sinePlaneShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "wavePlaneShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "flareCubeShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "softPlaneShape.iog" ":initialShadingGroup.dsm" -na;
// End of rig_deformers.ma
