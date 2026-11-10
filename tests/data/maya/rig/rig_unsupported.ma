//Maya ASCII 2026 scene
//Name: rig_unsupported.ma
//Last modified: Tue, Oct 27, 2026 02:36:39 PM
//Codeset: 1252
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
fileInfo "product" "Maya 2026";
fileInfo "version" "2026";
fileInfo "cutIdentifier" "202510291147-60ec9eda33";
fileInfo "osv" "Windows 11 Pro v2009 (Build: 26200)";
fileInfo "UUID" "C627F9AC-4D6B-8373-B4A1-1486BD9E0C70";
createNode transform -s -n "persp";
	rename -uid "271E3F84-4EB3-7F88-18B2-38A9B4CBA5D4";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 28 21 28 ;
	setAttr ".r" -type "double3" -27.938352729602379 44.999999999999972 -5.172681101354183e-14 ;
createNode camera -s -n "perspShape" -p "persp";
	rename -uid "8F68A131-4531-03E6-2BA0-95B9B75C8A22";
	setAttr -k off ".v" no;
	setAttr ".fl" 34.999999999999993;
	setAttr ".coi" 44.82186966202994;
	setAttr ".imn" -type "string" "persp";
	setAttr ".den" -type "string" "persp_depth";
	setAttr ".man" -type "string" "persp_mask";
	setAttr ".hc" -type "string" "viewSet -p %camera";
createNode transform -s -n "top";
	rename -uid "CDFCF9EB-4199-EB8A-412B-6F87E4EE0F23";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 1000.1 0 ;
	setAttr ".r" -type "double3" -90 0 0 ;
createNode camera -s -n "topShape" -p "top";
	rename -uid "9C329D7F-4F27-C2A6-9AD7-CBB0DA2EE1CF";
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
	rename -uid "D10B2FC3-4C20-605A-D820-C99C56BA9F14";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 0 1000.1 ;
createNode camera -s -n "frontShape" -p "front";
	rename -uid "E5D5E35C-4C8A-B033-B1A7-3EB969CFB6FC";
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
	rename -uid "46C59CDB-4F66-C3D4-B9E3-3A9D391C1D3F";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 1000.1 0 0 ;
	setAttr ".r" -type "double3" 0 90 0 ;
createNode camera -s -n "sideShape" -p "side";
	rename -uid "C804DA13-4371-57B4-CC52-08915E7F0B9C";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "side";
	setAttr ".den" -type "string" "side_depth";
	setAttr ".man" -type "string" "side_mask";
	setAttr ".hc" -type "string" "viewSet -s %camera";
	setAttr ".o" yes;
createNode transform -n "ctl";
	rename -uid "37981AAF-4B34-8516-67C9-08BD8810FE5C";
	addAttr -ci true -sn "label" -ln "label" -dt "string";
createNode locator -n "ctlShape" -p "ctl";
	rename -uid "C7FF0276-4738-EB89-1ECA-DAA1A1381FCF";
	setAttr -k off ".v";
createNode transform -n "exprDriven";
	rename -uid "62C91AE2-4160-674D-724D-2CA4F7AC125A";
createNode locator -n "exprDrivenShape" -p "exprDriven";
	rename -uid "A3E897F9-4A3B-718C-8C43-ECA650378843";
	setAttr -k off ".v";
createNode joint -n "spine1";
	rename -uid "859404B5-4562-0582-3E23-5691811FA585";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
createNode joint -n "spine2" -p "spine1";
	rename -uid "6E2F5F7A-4B65-D09F-1187-7EA90DEF69CE";
	setAttr ".t" -type "double3" 0 2 0 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
createNode joint -n "spine3" -p "spine2";
	rename -uid "5561667F-4F13-248D-E5BF-CFA9CD04393A";
	setAttr ".t" -type "double3" 0 2 0 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
createNode joint -n "spine4" -p "spine3";
	rename -uid "7BE9F668-4A96-2127-BD48-95AC0F9A3FC8";
	setAttr ".t" -type "double3" 0 2 0 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
createNode ikEffector -n "effector1" -p "spine3";
	rename -uid "DF8E229E-4E1A-3894-C597-A89DAD8AF0A0";
	setAttr ".v" no;
	setAttr ".hd" yes;
createNode ikHandle -n "spineIk";
	rename -uid "A6643866-460A-3459-5911-B49B7592551F";
	setAttr ".t" -type "double3" 0 6 0 ;
	setAttr ".roc" yes;
createNode transform -n "curve1";
	rename -uid "662B0D51-4131-884A-8EE8-BFA3D76FD619";
createNode nurbsCurve -n "curveShape1" -p "curve1";
	rename -uid "6D88EBF4-4614-5BB7-65BC-57B55C9B29DC";
	setAttr -k off ".v";
	setAttr ".cc" -type "nurbsCurve" 
		3 1 0 no 3
		6 0 0 0 6 6 6
		4
		0 1.9017111907932151e-16 0
		-0 2.0000000277067738 -0
		0 3.9999999716641601 0
		0 6 0
		;
createNode transform -n "path";
	rename -uid "DE8AF3AA-4189-D2D4-99F9-B4937B95C5A0";
createNode nurbsCurve -n "curveShape2" -p "path";
	rename -uid "A8711F35-47E6-60F9-AE8C-E6916AC5FCD2";
	setAttr -k off ".v";
	setAttr ".cc" -type "nurbsCurve" 
		3 1 0 no 3
		6 0 0 0 1 1 1
		4
		0 0 0
		2 0 3
		5 0 4
		8 0 0
		;
createNode transform -n "positionMarker1" -p "curveShape2";
	rename -uid "16172C08-4E93-6005-3C4F-36B5773C0BBF";
createNode positionMarker -n "positionMarkerShape1" -p "positionMarker1";
	rename -uid "4EACA3CB-447F-7938-43F2-979D5D71B876";
	setAttr -k off ".v";
	setAttr ".uwo" yes;
	setAttr ".t" 1;
createNode transform -n "positionMarker2" -p "curveShape2";
	rename -uid "EE4C6ECE-42F1-DEBC-7EAE-1A955C16BE48";
createNode positionMarker -n "positionMarkerShape2" -p "positionMarker2";
	rename -uid "C1D60D06-4500-945F-5B95-0D83A1D8D044";
	setAttr -k off ".v";
	setAttr ".uwo" yes;
	setAttr ".lp" -type "double3" 1 0 0 ;
	setAttr ".t" 24;
createNode transform -n "rider";
	rename -uid "4A6CD6FE-4794-1CEC-7D6F-C28971ADBB21";
	setAttr ".smd" 7;
createNode locator -n "riderShape" -p "rider";
	rename -uid "8784EF49-4DD9-FCAF-1F30-82BF0393C079";
	setAttr -k off ".v";
createNode transform -n "bumpy";
	rename -uid "ACDC8F4A-42D0-8768-DBD0-81B729E674AC";
	setAttr ".rp" -type "double3" 0 0 -10 ;
	setAttr ".sp" -type "double3" 0 0 -10 ;
createNode mesh -n "bumpyShape" -p "bumpy";
	rename -uid "04322848-4EA5-FF44-8F42-4D9AF7F123E8";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "bumpyShapeOrig" -p "bumpy";
	rename -uid "781DA23B-4056-8703-19C5-55AC229CCC28";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 5 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "e[78:83]";
	setAttr ".gtag[1].gtagnm" -type "string" "front";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 6 "e[0]" "e[2]" "e[4]" "e[6]" "e[8]" "e[10]";
	setAttr ".gtag[2].gtagnm" -type "string" "left";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 6 "e[1]" "e[14]" "e[27]" "e[40]" "e[53]" "e[66]";
	setAttr ".gtag[3].gtagnm" -type "string" "right";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 6 "e[12]" "e[25]" "e[38]" "e[51]" "e[64]" "e[77]";
	setAttr ".gtag[4].gtagnm" -type "string" "rim";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 16 "e[0:2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[12]" "e[14]" "e[25]" "e[27]" "e[38]" "e[40]" "e[51]" "e[53]" "e[64]" "e[66]" "e[77:83]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 49 ".uvst[0].uvsp[0:48]" -type "float2" 0 0 0.16666667 0
		 0.33333334 0 0.5 0 0.66666669 0 0.83333337 0 1 0 0 0.16666667 0.16666667 0.16666667
		 0.33333334 0.16666667 0.5 0.16666667 0.66666669 0.16666667 0.83333337 0.16666667
		 1 0.16666667 0 0.33333334 0.16666667 0.33333334 0.33333334 0.33333334 0.5 0.33333334
		 0.66666669 0.33333334 0.83333337 0.33333334 1 0.33333334 0 0.5 0.16666667 0.5 0.33333334
		 0.5 0.5 0.5 0.66666669 0.5 0.83333337 0.5 1 0.5 0 0.66666669 0.16666667 0.66666669
		 0.33333334 0.66666669 0.5 0.66666669 0.66666669 0.66666669 0.83333337 0.66666669
		 1 0.66666669 0 0.83333337 0.16666667 0.83333337 0.33333334 0.83333337 0.5 0.83333337
		 0.66666669 0.83333337 0.83333337 0.83333337 1 0.83333337 0 1 0.16666667 1 0.33333334
		 1 0.5 1 0.66666669 1 0.83333337 1 1 1;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 49 ".pt[0:48]" -type "float3"  0 0 -10 0 0 -10 0 0 -10 0 
		0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 
		-10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 
		0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 
		0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 -10 0 0 
		-10 0 0 -10 0 0 -10 0 0 -10 0 0 -10;
	setAttr -s 49 ".vt[0:48]"  -2 0 2 -1.33333325 0 2 -0.66666663 0 2 0 0 2
		 0.66666675 0 2 1.33333349 0 2 2 0 2 -2 0 1.33333325 -1.33333325 0 1.33333325 -0.66666663 0 1.33333325
		 0 0 1.33333325 0.66666675 0 1.33333325 1.33333349 0 1.33333325 2 0 1.33333325 -2 0 0.66666663
		 -1.33333325 0 0.66666663 -0.66666663 0 0.66666663 0 0 0.66666663 0.66666675 0 0.66666663
		 1.33333349 0 0.66666663 2 0 0.66666663 -2 0 0 -1.33333325 0 0 -0.66666663 0 0 0 0 0
		 0.66666675 0 0 1.33333349 0 0 2 0 0 -2 0 -0.66666675 -1.33333325 0 -0.66666675 -0.66666663 0 -0.66666675
		 0 0 -0.66666675 0.66666675 0 -0.66666675 1.33333349 0 -0.66666675 2 0 -0.66666675
		 -2 0 -1.33333349 -1.33333325 0 -1.33333349 -0.66666663 0 -1.33333349 0 0 -1.33333349
		 0.66666675 0 -1.33333349 1.33333349 0 -1.33333349 2 0 -1.33333349 -2 0 -2 -1.33333325 0 -2
		 -0.66666663 0 -2 0 0 -2 0.66666675 0 -2 1.33333349 0 -2 2 0 -2;
	setAttr -s 84 ".ed[0:83]"  0 1 0 0 7 0 1 2 0 1 8 1 2 3 0 2 9 1 3 4 0
		 3 10 1 4 5 0 4 11 1 5 6 0 5 12 1 6 13 0 7 8 1 7 14 0 8 9 1 8 15 1 9 10 1 9 16 1 10 11 1
		 10 17 1 11 12 1 11 18 1 12 13 1 12 19 1 13 20 0 14 15 1 14 21 0 15 16 1 15 22 1 16 17 1
		 16 23 1 17 18 1 17 24 1 18 19 1 18 25 1 19 20 1 19 26 1 20 27 0 21 22 1 21 28 0 22 23 1
		 22 29 1 23 24 1 23 30 1 24 25 1 24 31 1 25 26 1 25 32 1 26 27 1 26 33 1 27 34 0 28 29 1
		 28 35 0 29 30 1 29 36 1 30 31 1 30 37 1 31 32 1 31 38 1 32 33 1 32 39 1 33 34 1 33 40 1
		 34 41 0 35 36 1 35 42 0 36 37 1 36 43 1 37 38 1 37 44 1 38 39 1 38 45 1 39 40 1 39 46 1
		 40 41 1 40 47 1 41 48 0 42 43 0 43 44 0 44 45 0 45 46 0 46 47 0 47 48 0;
	setAttr -s 36 -ch 144 ".fc[0:35]" -type "polyFaces" 
		f 4 0 3 -14 -2
		mu 0 4 0 1 8 7
		f 4 2 5 -16 -4
		mu 0 4 1 2 9 8
		f 4 4 7 -18 -6
		mu 0 4 2 3 10 9
		f 4 6 9 -20 -8
		mu 0 4 3 4 11 10
		f 4 8 11 -22 -10
		mu 0 4 4 5 12 11
		f 4 10 12 -24 -12
		mu 0 4 5 6 13 12
		f 4 13 16 -27 -15
		mu 0 4 7 8 15 14
		f 4 15 18 -29 -17
		mu 0 4 8 9 16 15
		f 4 17 20 -31 -19
		mu 0 4 9 10 17 16
		f 4 19 22 -33 -21
		mu 0 4 10 11 18 17
		f 4 21 24 -35 -23
		mu 0 4 11 12 19 18
		f 4 23 25 -37 -25
		mu 0 4 12 13 20 19
		f 4 26 29 -40 -28
		mu 0 4 14 15 22 21
		f 4 28 31 -42 -30
		mu 0 4 15 16 23 22
		f 4 30 33 -44 -32
		mu 0 4 16 17 24 23
		f 4 32 35 -46 -34
		mu 0 4 17 18 25 24
		f 4 34 37 -48 -36
		mu 0 4 18 19 26 25
		f 4 36 38 -50 -38
		mu 0 4 19 20 27 26
		f 4 39 42 -53 -41
		mu 0 4 21 22 29 28
		f 4 41 44 -55 -43
		mu 0 4 22 23 30 29
		f 4 43 46 -57 -45
		mu 0 4 23 24 31 30
		f 4 45 48 -59 -47
		mu 0 4 24 25 32 31
		f 4 47 50 -61 -49
		mu 0 4 25 26 33 32
		f 4 49 51 -63 -51
		mu 0 4 26 27 34 33
		f 4 52 55 -66 -54
		mu 0 4 28 29 36 35
		f 4 54 57 -68 -56
		mu 0 4 29 30 37 36
		f 4 56 59 -70 -58
		mu 0 4 30 31 38 37
		f 4 58 61 -72 -60
		mu 0 4 31 32 39 38
		f 4 60 63 -74 -62
		mu 0 4 32 33 40 39
		f 4 62 64 -76 -64
		mu 0 4 33 34 41 40
		f 4 65 68 -79 -67
		mu 0 4 35 36 43 42
		f 4 67 70 -80 -69
		mu 0 4 36 37 44 43
		f 4 69 72 -81 -71
		mu 0 4 37 38 45 44
		f 4 71 74 -82 -73
		mu 0 4 38 39 46 45
		f 4 73 76 -83 -75
		mu 0 4 39 40 47 46
		f 4 75 77 -84 -77
		mu 0 4 40 41 48 47;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "sheet";
	rename -uid "76280E62-4112-D3F9-CA58-AC8408ED5046";
	setAttr ".rp" -type "double3" 0 0 10 ;
	setAttr ".sp" -type "double3" 0 0 10 ;
createNode mesh -n "sheetShape" -p "sheet";
	rename -uid "CDA92101-47EC-3EB7-E555-2799F28C9556";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
createNode mesh -n "sheetShapeOrig" -p "sheet";
	rename -uid "DF42F3FB-4BF7-5EA1-982F-42B333ADC357";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 5 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "e[78:83]";
	setAttr ".gtag[1].gtagnm" -type "string" "front";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 6 "e[0]" "e[2]" "e[4]" "e[6]" "e[8]" "e[10]";
	setAttr ".gtag[2].gtagnm" -type "string" "left";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 6 "e[1]" "e[14]" "e[27]" "e[40]" "e[53]" "e[66]";
	setAttr ".gtag[3].gtagnm" -type "string" "right";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 6 "e[12]" "e[25]" "e[38]" "e[51]" "e[64]" "e[77]";
	setAttr ".gtag[4].gtagnm" -type "string" "rim";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 16 "e[0:2]" "e[4]" "e[6]" "e[8]" "e[10]" "e[12]" "e[14]" "e[25]" "e[27]" "e[38]" "e[40]" "e[51]" "e[53]" "e[64]" "e[66]" "e[77:83]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 49 ".uvst[0].uvsp[0:48]" -type "float2" 0 0 0.16666667 0
		 0.33333334 0 0.5 0 0.66666669 0 0.83333337 0 1 0 0 0.16666667 0.16666667 0.16666667
		 0.33333334 0.16666667 0.5 0.16666667 0.66666669 0.16666667 0.83333337 0.16666667
		 1 0.16666667 0 0.33333334 0.16666667 0.33333334 0.33333334 0.33333334 0.5 0.33333334
		 0.66666669 0.33333334 0.83333337 0.33333334 1 0.33333334 0 0.5 0.16666667 0.5 0.33333334
		 0.5 0.5 0.5 0.66666669 0.5 0.83333337 0.5 1 0.5 0 0.66666669 0.16666667 0.66666669
		 0.33333334 0.66666669 0.5 0.66666669 0.66666669 0.66666669 0.83333337 0.66666669
		 1 0.66666669 0 0.83333337 0.16666667 0.83333337 0.33333334 0.83333337 0.5 0.83333337
		 0.66666669 0.83333337 0.83333337 0.83333337 1 0.83333337 0 1 0.16666667 1 0.33333334
		 1 0.5 1 0.66666669 1 0.83333337 1 1 1;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 49 ".pt[0:48]" -type "float3"  0 0 10 0 0 10 0 0 10 0 0 
		10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 
		0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 
		0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 
		10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10 0 0 10;
	setAttr -s 49 ".vt[0:48]"  -2 0 2 -1.33333325 0 2 -0.66666663 0 2 0 0 2
		 0.66666675 0 2 1.33333349 0 2 2 0 2 -2 0 1.33333325 -1.33333325 0 1.33333325 -0.66666663 0 1.33333325
		 0 0 1.33333325 0.66666675 0 1.33333325 1.33333349 0 1.33333325 2 0 1.33333325 -2 0 0.66666663
		 -1.33333325 0 0.66666663 -0.66666663 0 0.66666663 0 0 0.66666663 0.66666675 0 0.66666663
		 1.33333349 0 0.66666663 2 0 0.66666663 -2 0 0 -1.33333325 0 0 -0.66666663 0 0 0 0 0
		 0.66666675 0 0 1.33333349 0 0 2 0 0 -2 0 -0.66666675 -1.33333325 0 -0.66666675 -0.66666663 0 -0.66666675
		 0 0 -0.66666675 0.66666675 0 -0.66666675 1.33333349 0 -0.66666675 2 0 -0.66666675
		 -2 0 -1.33333349 -1.33333325 0 -1.33333349 -0.66666663 0 -1.33333349 0 0 -1.33333349
		 0.66666675 0 -1.33333349 1.33333349 0 -1.33333349 2 0 -1.33333349 -2 0 -2 -1.33333325 0 -2
		 -0.66666663 0 -2 0 0 -2 0.66666675 0 -2 1.33333349 0 -2 2 0 -2;
	setAttr -s 84 ".ed[0:83]"  0 1 0 0 7 0 1 2 0 1 8 1 2 3 0 2 9 1 3 4 0
		 3 10 1 4 5 0 4 11 1 5 6 0 5 12 1 6 13 0 7 8 1 7 14 0 8 9 1 8 15 1 9 10 1 9 16 1 10 11 1
		 10 17 1 11 12 1 11 18 1 12 13 1 12 19 1 13 20 0 14 15 1 14 21 0 15 16 1 15 22 1 16 17 1
		 16 23 1 17 18 1 17 24 1 18 19 1 18 25 1 19 20 1 19 26 1 20 27 0 21 22 1 21 28 0 22 23 1
		 22 29 1 23 24 1 23 30 1 24 25 1 24 31 1 25 26 1 25 32 1 26 27 1 26 33 1 27 34 0 28 29 1
		 28 35 0 29 30 1 29 36 1 30 31 1 30 37 1 31 32 1 31 38 1 32 33 1 32 39 1 33 34 1 33 40 1
		 34 41 0 35 36 1 35 42 0 36 37 1 36 43 1 37 38 1 37 44 1 38 39 1 38 45 1 39 40 1 39 46 1
		 40 41 1 40 47 1 41 48 0 42 43 0 43 44 0 44 45 0 45 46 0 46 47 0 47 48 0;
	setAttr -s 36 -ch 144 ".fc[0:35]" -type "polyFaces" 
		f 4 0 3 -14 -2
		mu 0 4 0 1 8 7
		f 4 2 5 -16 -4
		mu 0 4 1 2 9 8
		f 4 4 7 -18 -6
		mu 0 4 2 3 10 9
		f 4 6 9 -20 -8
		mu 0 4 3 4 11 10
		f 4 8 11 -22 -10
		mu 0 4 4 5 12 11
		f 4 10 12 -24 -12
		mu 0 4 5 6 13 12
		f 4 13 16 -27 -15
		mu 0 4 7 8 15 14
		f 4 15 18 -29 -17
		mu 0 4 8 9 16 15
		f 4 17 20 -31 -19
		mu 0 4 9 10 17 16
		f 4 19 22 -33 -21
		mu 0 4 10 11 18 17
		f 4 21 24 -35 -23
		mu 0 4 11 12 19 18
		f 4 23 25 -37 -25
		mu 0 4 12 13 20 19
		f 4 26 29 -40 -28
		mu 0 4 14 15 22 21
		f 4 28 31 -42 -30
		mu 0 4 15 16 23 22
		f 4 30 33 -44 -32
		mu 0 4 16 17 24 23
		f 4 32 35 -46 -34
		mu 0 4 17 18 25 24
		f 4 34 37 -48 -36
		mu 0 4 18 19 26 25
		f 4 36 38 -50 -38
		mu 0 4 19 20 27 26
		f 4 39 42 -53 -41
		mu 0 4 21 22 29 28
		f 4 41 44 -55 -43
		mu 0 4 22 23 30 29
		f 4 43 46 -57 -45
		mu 0 4 23 24 31 30
		f 4 45 48 -59 -47
		mu 0 4 24 25 32 31
		f 4 47 50 -61 -49
		mu 0 4 25 26 33 32
		f 4 49 51 -63 -51
		mu 0 4 26 27 34 33
		f 4 52 55 -66 -54
		mu 0 4 28 29 36 35
		f 4 54 57 -68 -56
		mu 0 4 29 30 37 36
		f 4 56 59 -70 -58
		mu 0 4 30 31 38 37
		f 4 58 61 -72 -60
		mu 0 4 31 32 39 38
		f 4 60 63 -74 -62
		mu 0 4 32 33 40 39
		f 4 62 64 -76 -64
		mu 0 4 33 34 41 40
		f 4 65 68 -79 -67
		mu 0 4 35 36 43 42
		f 4 67 70 -80 -69
		mu 0 4 36 37 44 43
		f 4 69 72 -81 -71
		mu 0 4 37 38 45 44
		f 4 71 74 -82 -73
		mu 0 4 38 39 46 45
		f 4 73 76 -83 -75
		mu 0 4 39 40 47 46
		f 4 75 77 -84 -77
		mu 0 4 40 41 48 47;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "wireCurve";
	rename -uid "F147371F-4EF7-577F-CDEF-51812A49A8F9";
createNode nurbsCurve -n "curveShape3" -p "wireCurve";
	rename -uid "19015E65-4E8C-F03F-0624-A9B4718684DC";
	setAttr -k off ".v";
	setAttr ".cc" -type "nurbsCurve" 
		1 1 0 no 3
		2 0 1
		2
		-2 0 10
		2 0 10
		;
createNode transform -n "wireCurveBaseWire";
	rename -uid "D30F04ED-43D9-F68A-9F29-2288FBE06925";
	setAttr ".v" no;
createNode nurbsCurve -n "curveShape3" -p "wireCurveBaseWire";
	rename -uid "E2953975-4A8C-8DB9-E161-58B42D0F2FA0";
	setAttr -k off ".v";
	setAttr ".cc" -type "nurbsCurve" 
		1 1 0 no 3
		2 0 1
		2
		-2 0 10
		2 0 10
		;
createNode lightLinker -s -n "lightLinker1";
	rename -uid "3886E8F5-4205-A97F-A3F4-50B835D24090";
	setAttr -s 2 ".lnk";
	setAttr -s 2 ".slnk";
createNode displayLayerManager -n "layerManager";
	rename -uid "07E96B2D-44AF-F3B8-1D0A-828B6100301C";
createNode displayLayer -n "defaultLayer";
	rename -uid "D6C1CC27-4E1E-749D-4781-93A1181B9FDC";
	setAttr ".ufem" -type "stringArray" 0  ;
createNode renderLayerManager -n "renderLayerManager";
	rename -uid "13F625F4-49EE-72D4-DA2A-2D9C29A653B8";
createNode renderLayer -n "defaultRenderLayer";
	rename -uid "E87AEECC-4C29-E4F4-7237-97BB931CC206";
	setAttr ".g" yes;
createNode expression -n "wobble";
	rename -uid "E0C3FBC3-4BA3-A16B-B76E-33BBE6F3783C";
	setAttr -k on ".nds";
	setAttr ".ixp" -type "string" ".O[0] = sin(time) * .I[0];";
createNode ikSplineSolver -n "ikSplineSolver";
	rename -uid "B6F54A25-4BF6-9019-E896-E48878F1742D";
createNode motionPath -n "riderPath";
	rename -uid "140089C6-4977-BD05-F59D-A897F1061F35";
	setAttr -l on ".ft";
	setAttr -l on ".ut";
	setAttr -l on ".st";
	setAttr -s 2 ".pmt";
	setAttr -s 2 ".pmt";
createNode animCurveTL -n "riderPath_uValue";
	rename -uid "68A88FC5-4CC4-2D9F-F044-8AB11511A1D5";
	setAttr ".tan" 18;
	setAttr ".wgt" no;
	setAttr -s 2 ".ktv[0:1]"  1 0 24 1;
createNode addDL -n "addDL1";
	rename -uid "0D453FF2-4381-7027-3F1A-71B16DEF62D2";
	setAttr ".ihi" 2;
createNode addDL -n "addDL2";
	rename -uid "DA57A3AD-4D48-02C2-5F86-4CAE7B6DA1AE";
	setAttr ".ihi" 2;
createNode addDL -n "addDL3";
	rename -uid "174AE333-41A3-9CDC-7714-DDAF4AD91666";
	setAttr ".ihi" 2;
createNode curveInfo -n "pathInfo";
	rename -uid "AE42FFAC-4E35-EC17-7FDB-9DAE708E57F0";
createNode file -n "bumpTex";
	rename -uid "7A4A51B9-4370-5554-1725-A6A39801F7F8";
	setAttr ".ftn" -type "string" "X:/Lila-wt/mayarig/tests/data/maya/rig/../sourceimages/front.png";
	setAttr ".cs" -type "string" "sRGB Encoded Rec.709 (sRGB)";
createNode textureDeformer -n "bumpDeform";
	rename -uid "E626F702-4DD9-05C1-D8CC-A2B856593166";
	setAttr ".s" 0.5;
	setAttr ".o" -0.25;
createNode wire -n "sheetWire";
	rename -uid "85A2CF24-4D98-3459-2AB4-0090C54F3E11";
	setAttr ".dds[0]"  1;
	setAttr ".sc[0]"  1;
createNode script -n "uiConfigurationScriptNode";
	rename -uid "5B345B45-4568-1337-CFC0-FB91A09DC281";
	setAttr ".b" -type "string" "// Maya Mel UI Configuration File.\n// No UI generated in batch mode.\n";
	setAttr ".st" 3;
createNode script -n "sceneConfigurationScriptNode";
	rename -uid "D9232E2B-4BE1-CE78-D983-2784EE5CBCB9";
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
select -ne :defaultTextureList1;
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
select -ne :ikSystem;
	setAttr -s 4 ".sol";
connectAttr "wobble.out[0]" "exprDriven.ty";
connectAttr "spine1.s" "spine2.is";
connectAttr "spine2.s" "spine3.is";
connectAttr "spine3.s" "spine4.is";
connectAttr "spine4.tx" "effector1.tx";
connectAttr "spine4.ty" "effector1.ty";
connectAttr "spine4.tz" "effector1.tz";
connectAttr "spine4.opm" "effector1.opm";
connectAttr "spine1.msg" "spineIk.hsj";
connectAttr "effector1.hp" "spineIk.hee";
connectAttr "ikSplineSolver.msg" "spineIk.hsv";
connectAttr "curveShape1.ws" "spineIk.ic";
connectAttr "addDL1.o" "rider.tx";
connectAttr "addDL2.o" "rider.ty";
connectAttr "addDL3.o" "rider.tz";
connectAttr "riderPath.msg" "rider.sml";
connectAttr "bumpDeform.og[0]" "bumpyShape.i";
connectAttr "sheetWire.og[0]" "sheetShape.i";
relationship "link" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
connectAttr "layerManager.dli[0]" "defaultLayer.id";
connectAttr "renderLayerManager.rlmi[0]" "defaultRenderLayer.rlid";
connectAttr ":time1.o" "wobble.tim";
connectAttr "ctl.tx" "wobble.in[0]";
connectAttr "riderPath_uValue.o" "riderPath.u";
connectAttr "curveShape2.ws" "riderPath.gp";
connectAttr "positionMarkerShape1.t" "riderPath.pmt[0]";
connectAttr "positionMarkerShape2.t" "riderPath.pmt[1]";
connectAttr "rider.tmrx" "addDL1.i1";
connectAttr "riderPath.xc" "addDL1.i2";
connectAttr "rider.tmry" "addDL2.i1";
connectAttr "riderPath.yc" "addDL2.i2";
connectAttr "rider.tmrz" "addDL3.i1";
connectAttr "riderPath.zc" "addDL3.i2";
connectAttr "curveShape2.ws" "pathInfo.ic";
connectAttr ":defaultColorMgtGlobals.cme" "bumpTex.cme";
connectAttr ":defaultColorMgtGlobals.cfe" "bumpTex.cmcf";
connectAttr ":defaultColorMgtGlobals.cfp" "bumpTex.cmcp";
connectAttr ":defaultColorMgtGlobals.wsn" "bumpTex.ws";
connectAttr "bumpyShapeOrig.w" "bumpDeform.ip[0].ig";
connectAttr "bumpyShapeOrig.o" "bumpDeform.orggeom[0]";
connectAttr "bumpTex.oc" "bumpDeform.t";
connectAttr "sheetShapeOrig.w" "sheetWire.ip[0].ig";
connectAttr "sheetShapeOrig.o" "sheetWire.orggeom[0]";
connectAttr "|wireCurveBaseWire|curveShape3.ws" "sheetWire.bw[0]";
connectAttr "|wireCurve|curveShape3.ws" "sheetWire.dw[0]";
connectAttr "defaultRenderLayer.msg" ":defaultRenderingList1.r" -na;
connectAttr "bumpTex.msg" ":defaultTextureList1.tx" -na;
connectAttr "bumpyShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "sheetShape.iog" ":initialShadingGroup.dsm" -na;
connectAttr "ikSplineSolver.msg" ":ikSystem.sol" -na;
dataStructure -fmt "raw" -as "name=externalContentTable:string=node:string=key:string=upath:uint32=upathcrc:string=rpath:string=roles";
applyMetadata -fmt "raw" -v "channel\nname externalContentTable\nstream\nname v1.0\nindexType numeric\nstructure externalContentTable\n0\n\"bumpTex\" \"fileTextureName\" \"X:/Lila-wt/mayarig/tests/data/maya/rig/../sourceimages/front.png\" 169038634 \"X:/Lila-wt/mayarig/tests/data/maya/sourceimages/front.png\" \"sourceImages\"\nendStream\nendChannel\nendAssociations\n" 
		-scn;
// End of rig_unsupported.ma
