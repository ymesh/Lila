//Maya ASCII 2026 scene
//Name: rig_utility.ma
//Last modified: Tue, Oct 27, 2026 02:39:49 PM
//Codeset: 1252
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
fileInfo "product" "Maya 2026";
fileInfo "version" "2026";
fileInfo "cutIdentifier" "202510291147-60ec9eda33";
fileInfo "osv" "Windows 11 Pro v2009 (Build: 26200)";
fileInfo "UUID" "C0AFD934-49BC-2007-731A-3BA625016562";
createNode transform -s -n "persp";
	rename -uid "D2D36FB0-4447-2BD8-5DD4-79BCBF38F7B4";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 28 21 28 ;
	setAttr ".r" -type "double3" -27.938352729602379 44.999999999999972 -5.172681101354183e-14 ;
createNode camera -s -n "perspShape" -p "persp";
	rename -uid "1F7E6854-46B7-8DF7-A48E-01B4E704FF03";
	setAttr -k off ".v" no;
	setAttr ".fl" 34.999999999999993;
	setAttr ".coi" 44.82186966202994;
	setAttr ".imn" -type "string" "persp";
	setAttr ".den" -type "string" "persp_depth";
	setAttr ".man" -type "string" "persp_mask";
	setAttr ".hc" -type "string" "viewSet -p %camera";
createNode transform -s -n "top";
	rename -uid "0A6305C4-492D-8FE5-9CA9-E7B1E4196943";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 1000.1 0 ;
	setAttr ".r" -type "double3" -90 0 0 ;
createNode camera -s -n "topShape" -p "top";
	rename -uid "67A1228B-4320-5590-8CF2-8FAA602280C0";
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
	rename -uid "F1B124C7-41A2-66BE-2487-B6A5E8E95C05";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 0 1000.1 ;
createNode camera -s -n "frontShape" -p "front";
	rename -uid "463D325A-4E1D-E5D1-37E3-54BC860C2F6B";
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
	rename -uid "16E375AD-47C2-39C2-7F0A-D584C5D065D4";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 1000.1 0 0 ;
	setAttr ".r" -type "double3" 0 90 0 ;
createNode camera -s -n "sideShape" -p "side";
	rename -uid "A259728F-4F82-FD6F-2830-87A1FA649C95";
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
	rename -uid "1663535F-4000-88CE-DA88-F689BA37D122";
	addAttr -ci true -k true -sn "a" -ln "a" -at "double";
	addAttr -ci true -k true -sn "b" -ln "b" -at "double";
	addAttr -ci true -k true -sn "ang" -ln "ang" -at "doubleAngle";
	addAttr -ci true -k true -sn "sel" -ln "sel" -at "long";
	setAttr ".t" -type "double3" 1 2 3 ;
	setAttr ".r" -type "double3" 10 20 29.999999999999996 ;
	setAttr -k on ".a" 0.25;
	setAttr -k on ".b" 0.5;
	setAttr -k on ".ang" 14.999999999999998;
	setAttr -k on ".sel";
createNode locator -n "ctlShape" -p "ctl";
	rename -uid "2843C41E-4BD0-C399-73F5-978797FF2244";
	setAttr -k off ".v";
createNode transform -n "anchor";
	rename -uid "88406805-4A2E-448C-9033-FE992B700BF9";
	setAttr ".t" -type "double3" -2 1 4 ;
createNode locator -n "anchorShape" -p "anchor";
	rename -uid "8DA43E7E-43C1-2C74-1F23-0EB5132B7006";
	setAttr -k off ".v";
createNode transform -n "out1";
	rename -uid "98E77B1E-40E6-28AB-DC21-C9989335A458";
createNode locator -n "out1Shape" -p "out1";
	rename -uid "57319E7B-435E-7F54-E0E2-75932C69BAF3";
	setAttr -k off ".v";
createNode transform -n "out2";
	rename -uid "BE68AAEA-44E2-83E2-EB2C-E6BB17A921B6";
createNode locator -n "out2Shape" -p "out2";
	rename -uid "C7A2E6D6-4D5A-8B7F-E3B9-0F99C4E85E72";
	setAttr -k off ".v";
createNode transform -n "out3";
	rename -uid "0B665BA4-44A1-BBE8-E07B-95B3D5E4FF67";
createNode locator -n "out3Shape" -p "out3";
	rename -uid "C3DFE720-4429-8024-24F5-7D83830CF3E4";
	setAttr -k off ".v";
createNode transform -n "out4";
	rename -uid "CAD31F8C-4052-67A0-E69F-99B3FA721480";
createNode locator -n "out4Shape" -p "out4";
	rename -uid "59DA4FFD-471F-27C2-1B2D-EDB195F45D31";
	setAttr -k off ".v";
createNode transform -n "out5";
	rename -uid "A6DE4DAE-47D0-9EF8-4FEF-BDA29986E80C";
createNode locator -n "out5Shape" -p "out5";
	rename -uid "B22320F2-4DBA-DAFA-3F11-17A4000D9EEE";
	setAttr -k off ".v";
createNode transform -n "out6";
	rename -uid "3F89E6F3-4799-78FD-E844-8EAA3C6C9F7E";
createNode locator -n "out6Shape" -p "out6";
	rename -uid "104D7394-4F4F-AA82-B022-09996DC299E6";
	setAttr -k off ".v";
createNode transform -n "out7";
	rename -uid "C666670D-419B-C7CA-D6B2-8F9DCFF7B0F6";
createNode locator -n "out7Shape" -p "out7";
	rename -uid "359A653D-4334-54BA-D49D-BE8919736E9C";
	setAttr -k off ".v";
createNode transform -n "out8";
	rename -uid "985D899B-40B8-1DFC-AFF8-2EBF1BC7EC5C";
createNode locator -n "out8Shape" -p "out8";
	rename -uid "B508EB1D-4EEF-195C-2E4B-F099BEE74FF7";
	setAttr -k off ".v";
createNode transform -n "out9";
	rename -uid "53C08FE5-4BBE-C5AF-F15B-92A268547F9A";
createNode locator -n "out9Shape" -p "out9";
	rename -uid "24098112-4258-28F0-8A0A-32A042EA40DA";
	setAttr -k off ".v";
createNode transform -n "out10";
	rename -uid "56549434-4ADD-82AC-31E2-C98985A12674";
createNode locator -n "out10Shape" -p "out10";
	rename -uid "B708B16C-4963-78BC-9722-E58222B95265";
	setAttr -k off ".v";
createNode transform -n "out11";
	rename -uid "0F4E2972-4932-E900-7ABB-E9978AFB165B";
createNode locator -n "out11Shape" -p "out11";
	rename -uid "8DB2CF71-4B05-3538-6659-339308086B95";
	setAttr -k off ".v";
createNode transform -n "out12";
	rename -uid "E02AEFBD-4B17-1900-3911-508BE2A6588F";
createNode locator -n "out12Shape" -p "out12";
	rename -uid "A724E5F0-422C-A3F9-995C-0F92FA97F1B8";
	setAttr -k off ".v";
createNode transform -n "out13";
	rename -uid "17FBE6AB-4F81-05D7-5906-E58F25C2A1B9";
createNode locator -n "out13Shape" -p "out13";
	rename -uid "7CBD5A28-475A-E3DF-E6DD-0AB76D235998";
	setAttr -k off ".v";
createNode transform -n "out14";
	rename -uid "54D1646B-40D6-3337-58CF-58BB2C2F580C";
createNode locator -n "out14Shape" -p "out14";
	rename -uid "92C151C9-4E7F-07B5-4102-8BA3FADEC60A";
	setAttr -k off ".v";
createNode transform -n "out15";
	rename -uid "29B252C9-4FA3-E188-4970-8AB1DA02148F";
createNode locator -n "out15Shape" -p "out15";
	rename -uid "CFD55D06-4D3E-D313-4912-7293D5B4F162";
	setAttr -k off ".v";
createNode transform -n "out16";
	rename -uid "C2A80446-462A-9E0F-DDF8-B19C185F1F54";
createNode locator -n "out16Shape" -p "out16";
	rename -uid "50344C97-49DA-7CE9-69BF-6889EF735F70";
	setAttr -k off ".v";
createNode transform -n "out17";
	rename -uid "054BF780-44C6-6B26-ED67-9E9A605E0BB2";
createNode locator -n "out17Shape" -p "out17";
	rename -uid "68241CE1-4795-311D-5FE4-6888EC872C90";
	setAttr -k off ".v";
createNode transform -n "out18";
	rename -uid "498A276E-494F-2444-FADC-91939A793179";
createNode locator -n "out18Shape" -p "out18";
	rename -uid "C41B34D8-43FE-94C4-5897-A49FB34E8484";
	setAttr -k off ".v";
createNode transform -n "out19";
	rename -uid "45896183-4E2F-9FD5-423A-318E5A191171";
createNode locator -n "out19Shape" -p "out19";
	rename -uid "707D4F5B-439C-F6C2-2BDF-B287E5E5053B";
	setAttr -k off ".v";
createNode transform -n "cpomBall";
	rename -uid "E87806AF-429E-79A2-E1AE-FB8186AD400E";
	setAttr ".t" -type "double3" 0 1 0 ;
	setAttr ".r" -type "double3" 0 29.999999999999996 0 ;
createNode mesh -n "cpomBallShape" -p "cpomBall";
	rename -uid "AE213EE8-42E7-218A-992E-B8AA8D40D680";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[8:11]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[12:15]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0:3]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[20:23]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[16:19]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[4:7]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 39 ".uvst[0].uvsp[0:38]" -type "float2" 0.33000001 0 0.49666667
		 0 0.66333336 0 0.33000001 0.125 0.49666667 0.125 0.66333336 0.125 0.33000001 0.25
		 0.49666667 0.25 0.66333336 0.25 0.33000001 0.375 0.49666667 0.375 0.66333336 0.375
		 0.33000001 0.5 0.49666667 0.5 0.66333336 0.5 0.33000001 0.625 0.49666667 0.625 0.66333336
		 0.625 0.33000001 0.75 0.49666667 0.75 0.66333336 0.75 0.33000001 0.875 0.49666667
		 0.875 0.66333336 0.875 0.33000001 1 0.49666667 1 0.66333336 1 1 0 0.875 0 1 0.125
		 0.875 0.125 1 0.25 0.875 0.25 0 0 0.125 0 0 0.125 0.125 0.125 0 0.25 0.125 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 26 ".vt[0:25]"  -1.5 -1 2 0 -1 2 1.5 -1 2 -1.5 0 2 0 0 2
		 1.5 0 2 -1.5 1 2 0 1 2 1.5 1 2 -1.5 1 0 0 1 0 1.5 1 0 -1.5 1 -2 0 1 -2 1.5 1 -2 -1.5 0 -2
		 0 0 -2 1.5 0 -2 -1.5 -1 -2 0 -1 -2 1.5 -1 -2 -1.5 -1 0 0 -1 0 1.5 -1 0 1.5 0 0 -1.5 0 0;
	setAttr -s 48 ".ed[0:47]"  0 1 0 1 2 0 3 4 1 4 5 1 6 7 0 7 8 0 9 10 1
		 10 11 1 12 13 0 13 14 0 15 16 1 16 17 1 18 19 0 19 20 0 21 22 1 22 23 1 0 3 0 1 4 1
		 2 5 0 3 6 0 4 7 1 5 8 0 6 9 0 7 10 1 8 11 0 9 12 0 10 13 1 11 14 0 12 15 0 13 16 1
		 14 17 0 15 18 0 16 19 1 17 20 0 18 21 0 19 22 1 20 23 0 21 0 0 22 1 1 23 2 0 17 24 1
		 24 5 1 23 24 1 24 11 1 15 25 1 25 3 1 21 25 1 25 9 1;
	setAttr -s 24 -ch 96 ".fc[0:23]" -type "polyFaces" 
		f 4 0 17 -3 -17
		mu 0 4 0 1 4 3
		f 4 1 18 -4 -18
		mu 0 4 1 2 5 4
		f 4 2 20 -5 -20
		mu 0 4 3 4 7 6
		f 4 3 21 -6 -21
		mu 0 4 4 5 8 7
		f 4 4 23 -7 -23
		mu 0 4 6 7 10 9
		f 4 5 24 -8 -24
		mu 0 4 7 8 11 10
		f 4 6 26 -9 -26
		mu 0 4 9 10 13 12
		f 4 7 27 -10 -27
		mu 0 4 10 11 14 13
		f 4 8 29 -11 -29
		mu 0 4 12 13 16 15
		f 4 9 30 -12 -30
		mu 0 4 13 14 17 16
		f 4 10 32 -13 -32
		mu 0 4 15 16 19 18
		f 4 11 33 -14 -33
		mu 0 4 16 17 20 19
		f 4 12 35 -15 -35
		mu 0 4 18 19 22 21
		f 4 13 36 -16 -36
		mu 0 4 19 20 23 22
		f 4 14 38 -1 -38
		mu 0 4 21 22 25 24
		f 4 15 39 -2 -39
		mu 0 4 22 23 26 25
		f 4 -37 -34 40 -43
		mu 0 4 28 27 29 30
		f 4 -40 42 41 -19
		mu 0 4 2 28 30 5
		f 4 -41 -31 -28 -44
		mu 0 4 30 29 31 32
		f 4 -42 43 -25 -22
		mu 0 4 5 30 32 8
		f 4 34 46 -45 31
		mu 0 4 33 34 36 35
		f 4 37 16 -46 -47
		mu 0 4 34 0 3 36
		f 4 44 47 25 28
		mu 0 4 35 36 38 37
		f 4 45 19 22 -48
		mu 0 4 36 3 6 38;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode lightLinker -s -n "lightLinker1";
	rename -uid "BCB868F6-48A4-9E60-FE02-CF84A82B88E1";
	setAttr -s 2 ".lnk";
	setAttr -s 2 ".slnk";
createNode displayLayerManager -n "layerManager";
	rename -uid "4A171DBB-4BDA-308B-1C93-8A82F5D2057D";
createNode displayLayer -n "defaultLayer";
	rename -uid "9681D67A-40EE-FB73-AF42-C0953E4EE918";
	setAttr ".ufem" -type "stringArray" 0  ;
createNode renderLayerManager -n "renderLayerManager";
	rename -uid "C549A16B-44DE-F1A3-587F-E685E8475C6C";
createNode renderLayer -n "defaultRenderLayer";
	rename -uid "E668B9EB-4D99-F481-B886-ADA4D0ED3BC3";
	setAttr ".g" yes;
createNode multiplyDivide -n "md1";
	rename -uid "41998403-4EFA-63DE-16CB-B4A4B6FABD88";
	setAttr ".i2" -type "float3" 2 3 -1 ;
createNode multiplyDivide -n "md2";
	rename -uid "7C0EEF24-415A-4B8B-9259-6A8B6F0D106F";
	setAttr ".op" 2;
	setAttr ".i2" -type "float3" 4 1 1 ;
createNode multiplyDivide -n "md3";
	rename -uid "F0D0F9E8-431B-31C3-B0A6-1B873BF815F6";
	setAttr ".op" 3;
	setAttr ".i2" -type "float3" 2 1 1 ;
createNode plusMinusAverage -n "pma1";
	rename -uid "8E74C110-46E7-1870-DC4E-4CA32C6C799E";
	setAttr -s 3 ".i3[1:2]" -type "float3"  1 1 1 0 1 0;
	setAttr -s 2 ".i3";
createNode plusMinusAverage -n "pma2";
	rename -uid "EC8E45BB-4F2D-6006-9804-7E9F35797BDC";
	setAttr ".op" 2;
	setAttr -s 3 ".i1[2]"  0.125;
	setAttr -s 2 ".i1";
createNode plusMinusAverage -n "pma3";
	rename -uid "BCF5D617-4C23-6BA1-52DF-C1882296D2E6";
	setAttr ".op" 3;
	setAttr -s 2 ".i2[0:1]" -type "float2" 1 0 3 5;
createNode condition -n "cond1";
	rename -uid "6C086999-49FE-0F53-D293-C4AD7033D42A";
	setAttr ".op" 2;
	setAttr ".st" 0.5;
	setAttr ".cf" -type "float3" -1 -2 -3 ;
createNode reverse -n "rev1";
	rename -uid "2A4237EC-44C7-5C20-4166-98B857455A21";
createNode clamp -n "clamp1";
	rename -uid "C29BAFDF-45F7-F510-A511-729798342BB1";
	setAttr ".mn" -type "float3" -1 -1 -1 ;
	setAttr ".mx" -type "float3" 1 2 3 ;
createNode setRange -n "sr1";
	rename -uid "DBDA1C23-4390-25DF-0223-69B7A03653C2";
	setAttr ".n" -type "float3" 0 10 -1 ;
	setAttr ".m" -type "float3" 1 20 1 ;
	setAttr ".on" -type "float3" -2 -2 0 ;
	setAttr ".om" -type "float3" 2 4 6 ;
createNode remapValue -n "remap1";
	rename -uid "91249203-441E-9511-62D1-22B44458732A";
	setAttr ".imx" 2;
	setAttr ".omn" -1;
	setAttr ".omx" 3;
	setAttr -s 3 ".vl[0:2]"  0 0 1 0.5 1 1 1 0.2 1;
	setAttr -s 2 ".cl";
	setAttr ".cl[0].clp" 0;
	setAttr ".cl[0].clc" -type "float3" 0 0 0 ;
	setAttr ".cl[0].cli" 1;
	setAttr ".cl[1].clp" 1;
	setAttr ".cl[1].clc" -type "float3" 1 1 1 ;
	setAttr ".cl[1].cli" 1;
createNode blendColors -n "bc1";
	rename -uid "5AD763A3-4623-0FAD-76AF-17AA50316030";
	setAttr ".c2" -type "float3" 5 6 7 ;
createNode blendTwoAttr -n "b2a1";
	rename -uid "63A24CD3-4B00-475F-FB8F-56B4EBB69ADB";
	setAttr -s 2 ".i";
	setAttr -s 2 ".i";
createNode distanceBetween -n "dist1";
	rename -uid "36B30A4B-4A8E-F58D-B10E-DFABD0F9FC49";
	setAttr ".p2" -type "double3" 0 1 0 ;
createNode pairBlend -n "pb1";
	rename -uid "62CEED11-46AE-5CC3-20CC-B7A71C4F98F9";
	setAttr ".it2" -type "double3" 4 5 6 ;
	setAttr ".ir2" -type "double3" 29.999999999999996 0 45 ;
	setAttr ".ri" 1;
createNode choice -n "choice1";
	rename -uid "91E6BB86-432E-BF42-C4C5-1B9A12D86E17";
	setAttr -s 2 ".i";
createNode vectorProduct -n "vp1";
	rename -uid "987767E1-4E44-5431-F001-58BC844E489F";
	setAttr ".op" 2;
	setAttr ".i2" -type "float3" 0 1 0 ;
createNode vectorProduct -n "vp2";
	rename -uid "3FE86891-4650-B5BA-1540-AC98B46E0F47";
	setAttr ".op" 4;
	setAttr ".i1" -type "float3" 1 0 0 ;
createNode multiplyDivide -n "md4";
	rename -uid "240F414F-423C-5FA8-4E4E-6A8B9FF0B175";
	setAttr ".i2" -type "float3" 2 1 1 ;
createNode unitConversion -n "unitConversion1";
	rename -uid "AA5CED08-4D1D-655A-7A89-DB8D9599CB5B";
	setAttr ".cf" 57.295779513082323;
createNode unitConversion -n "unitConversion2";
	rename -uid "97124864-498F-F794-9880-AB8870D993EC";
	setAttr ".cf" 0.017453292519943295;
createNode unitConversion -n "unitConversion3";
	rename -uid "399E3725-4DCA-E757-73C1-5E85DE01A468";
	setAttr ".cf" 57.295779513082323;
createNode plusMinusAverage -n "pma4";
	rename -uid "9A1BE47B-428C-0429-8FD5-A18E62BD3D83";
	setAttr -s 2 ".i1";
	setAttr -s 2 ".i1";
createNode unitConversion -n "unitConversion4";
	rename -uid "400BE67B-4458-A0D6-7260-B58F55768A5C";
	setAttr ".cf" 57.295779513082323;
createNode unitConversion -n "unitConversion5";
	rename -uid "A80D6DF7-477A-0532-9557-2681322527DB";
	setAttr ".cf" 57.295779513082323;
createNode unitConversion -n "unitConversion6";
	rename -uid "4FA3D860-4F91-817D-E12A-0AA32C84CF93";
	setAttr ".cf" 0.017453292519943295;
createNode setRange -n "sr2";
	rename -uid "EA945CEE-43C4-0705-6802-24BEA153611B";
	setAttr ".m" -type "float3" 90 0 0 ;
	setAttr ".om" -type "float3" 1 0 0 ;
createNode unitConversion -n "unitConversion7";
	rename -uid "75ECAE53-4CD3-52B1-B4B6-2EBCF4C3436C";
	setAttr ".cf" 0.017453292519943295;
createNode closestPointOnMesh -n "cpom1";
	rename -uid "12E2F2B3-4D41-AA62-C478-3A9ACD9C3910";
createNode script -n "uiConfigurationScriptNode";
	rename -uid "7F89E21E-4FBB-4997-53C6-01964CD3220E";
	setAttr ".b" -type "string" "// Maya Mel UI Configuration File.\n// No UI generated in batch mode.\n";
	setAttr ".st" 3;
createNode script -n "sceneConfigurationScriptNode";
	rename -uid "881FDB3A-4246-9FED-8A73-96B1C5B36164";
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
connectAttr "md1.o" "out1.t";
connectAttr "md2.ox" "out2.tx";
connectAttr "md3.ox" "out2.ty";
connectAttr "pma1.o3" "out3.t";
connectAttr "pma2.o1" "out3.sx";
connectAttr "pma3.o2x" "out3.sy";
connectAttr "pma3.o2y" "out3.sz";
connectAttr "cond1.oc" "out4.t";
connectAttr "rev1.o" "out5.t";
connectAttr "clamp1.op" "out6.t";
connectAttr "sr1.o" "out7.t";
connectAttr "remap1.ov" "out8.tx";
connectAttr "bc1.op" "out9.t";
connectAttr "b2a1.o" "out10.tx";
connectAttr "dist1.d" "out11.tx";
connectAttr "pb1.ot" "out12.t";
connectAttr "pb1.or" "out12.r";
connectAttr "choice1.o" "out13.tx";
connectAttr "vp1.o" "out14.t";
connectAttr "vp2.o" "out15.t";
connectAttr "unitConversion2.o" "out16.rz";
connectAttr "md4.oy" "out16.tx";
connectAttr "unitConversion6.o" "out17.ry";
connectAttr "unitConversion7.o" "out18.rx";
connectAttr "cpom1.p" "out19.t";
relationship "link" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
connectAttr "layerManager.dli[0]" "defaultLayer.id";
connectAttr "renderLayerManager.rlmi[0]" "defaultRenderLayer.rlid";
connectAttr "ctl.t" "md1.i1";
connectAttr "ctl.a" "md2.i1x";
connectAttr "ctl.b" "md3.i1x";
connectAttr "ctl.t" "pma1.i3[0]";
connectAttr "ctl.tx" "pma1.i3[2].i3y";
connectAttr "ctl.a" "pma2.i1[0]";
connectAttr "ctl.b" "pma2.i1[1]";
connectAttr "ctl.tx" "pma3.i2[0].i2x";
connectAttr "ctl.a" "cond1.ft";
connectAttr "ctl.t" "cond1.ct";
connectAttr "ctl.t" "rev1.i";
connectAttr "ctl.t" "clamp1.ip";
connectAttr "ctl.t" "sr1.v";
connectAttr "ctl.tx" "remap1.i";
connectAttr "ctl.b" "bc1.b";
connectAttr "ctl.t" "bc1.c1";
connectAttr "ctl.tx" "b2a1.i[0]";
connectAttr "ctl.ty" "b2a1.i[1]";
connectAttr "ctl.b" "b2a1.ab";
connectAttr "ctl.wm" "dist1.im1";
connectAttr "anchor.wm" "dist1.im2";
connectAttr "ctl.t" "pb1.it1";
connectAttr "ctl.r" "pb1.ir1";
connectAttr "ctl.b" "pb1.w";
connectAttr "ctl.sel" "choice1.s";
connectAttr "ctl.tx" "choice1.i[0]";
connectAttr "ctl.ty" "choice1.i[1]";
connectAttr "ctl.t" "vp1.i1";
connectAttr "ctl.wm" "vp2.m";
connectAttr "unitConversion1.o" "md4.i1x";
connectAttr "unitConversion3.o" "md4.i1y";
connectAttr "ctl.ang" "unitConversion1.i";
connectAttr "md4.ox" "unitConversion2.i";
connectAttr "ctl.rx" "unitConversion3.i";
connectAttr "unitConversion4.o" "pma4.i1[0]";
connectAttr "unitConversion5.o" "pma4.i1[1]";
connectAttr "ctl.rx" "unitConversion4.i";
connectAttr "ctl.ry" "unitConversion5.i";
connectAttr "pma4.o1" "unitConversion6.i";
connectAttr "ctl.b" "sr2.vx";
connectAttr "sr2.ox" "unitConversion7.i";
connectAttr "cpomBallShape.w" "cpom1.im";
connectAttr "ctl.t" "cpom1.ip";
connectAttr "defaultRenderLayer.msg" ":defaultRenderingList1.r" -na;
connectAttr "cpomBallShape.iog" ":initialShadingGroup.dsm" -na;
// End of rig_utility.ma
