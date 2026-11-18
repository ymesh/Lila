//Maya ASCII 2026 scene
//Name: history_replay.ma
//Hand-written: history Lila replays (delete faces, separate, extract, combine, merge vertices) and one it can't.
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
createNode transform -n "strip";
createNode mesh -n "stripOrigShape" -p "strip";
	setAttr ".io" yes;
	setAttr -s 8 ".vt[0:7]"  0 0 0 1 0 0 2 0 0 3 0 0 0 1 0 1 1 0 2 1 0 3 1 0;
	setAttr -s 10 ".ed[0:9]"  0 1 0 1 2 0 2 3 0 4 5 0 5 6 0 6 7 0 0 4 0 1 5 0 2 6 0 3 7 0;
	setAttr -s 3 -ch 12 ".fc[0:2]" -type "polyFaces"
		f 4 0 7 -4 -7
		f 4 1 8 -5 -8
		f 4 2 9 -6 -9;
createNode transform -n "partA";
createNode mesh -n "partAShape" -p "partA";
createNode transform -n "partB";
createNode mesh -n "partBShape" -p "partB";
	setAttr ".pt[0]" -type "float3"  0 0 1;
createNode deleteComponent -n "deleteComponent1";
	setAttr ".dc" -type "componentList" 1 "f[1]";
createNode polySeparate -n "polySeparate1";
	setAttr ".ic" 2;
createNode groupParts -n "groupParts1";
	setAttr ".ic" -type "componentList" 1 "f[0]";
createNode groupParts -n "groupParts2";
	setAttr ".ic" -type "componentList" 1 "f[0]";

createNode transform -n "strip2";
createNode mesh -n "strip2OrigShape" -p "strip2";
	setAttr ".io" yes;
	setAttr -s 8 ".vt[0:7]"  0 0 0 1 0 0 2 0 0 3 0 0 0 1 0 1 1 0 2 1 0 3 1 0;
	setAttr -s 10 ".ed[0:9]"  0 1 0 1 2 0 2 3 0 4 5 0 5 6 0 6 7 0 0 4 0 1 5 0 2 6 0 3 7 0;
	setAttr -s 3 -ch 12 ".fc[0:2]" -type "polyFaces"
		f 4 0 7 -4 -7
		f 4 1 8 -5 -8
		f 4 2 9 -6 -9;
createNode transform -n "chip";
createNode mesh -n "chipShape" -p "chip";
createNode transform -n "rest";
createNode mesh -n "restShape" -p "rest";
createNode polyChipOff -n "polyChipOff1";
	setAttr ".ics" -type "componentList" 1 "f[2]";
	setAttr ".dup" no;
createNode polySeparate -n "polySeparate2";
	setAttr ".ic" 2;
	setAttr ".rs" -type "Int32Array" 0 ;
createNode groupParts -n "groupParts3";
	setAttr ".ic" -type "componentList" 1 "f[0]";
createNode groupParts -n "groupParts4";
	setAttr ".ic" -type "componentList" 1 "f[0:1]";

createNode transform -n "quadA";
createNode mesh -n "quadAShape" -p "quadA";
	setAttr ".io" yes;
	setAttr -s 4 ".vt[0:3]"  0 0 0 1 0 0 1 1 0 0 1 0;
	setAttr -s 4 ".ed[0:3]"  0 1 0 1 2 0 2 3 0 3 0 0;
	setAttr -ch 4 ".fc[0]" -type "polyFaces"
		f 4 0 1 2 3;
createNode transform -n "quadB";
	setAttr ".t" -type "double3" 1 0 0 ;
createNode mesh -n "quadBShape" -p "quadB";
	setAttr ".io" yes;
	setAttr -s 4 ".vt[0:3]"  0 0 0 1 0 0 1 1 0 0 1 0;
	setAttr -s 4 ".ed[0:3]"  0 1 0 1 2 0 2 3 0 3 0 0;
	setAttr -ch 4 ".fc[0]" -type "polyFaces"
		f 4 0 1 2 3;
createNode transform -n "joined";
createNode mesh -n "joinedShape" -p "joined";
createNode polyUnite -n "polyUnite1";
	setAttr -s 2 ".ip";
	setAttr -s 2 ".im";
createNode polyMergeVert -n "polyMergeVert1";
	setAttr ".ics" -type "componentList" 1 "vtx[0:7]";
	setAttr ".d" 0.001;

createNode transform -n "strip3";
createNode mesh -n "strip3OrigShape" -p "strip3";
	setAttr ".io" yes;
	setAttr -s 4 ".vt[0:3]"  0 0 0 1 0 0 1 1 0 0 1 0;
	setAttr -s 4 ".ed[0:3]"  0 1 0 1 2 0 2 3 0 3 0 0;
	setAttr -ch 4 ".fc[0]" -type "polyFaces"
		f 4 0 1 2 3;
createNode transform -n "ringed";
createNode mesh -n "ringedShape" -p "ringed";
createNode polySplitRing -n "polySplitRing1";
	setAttr ".ics" -type "componentList" 1 "e[0]";

connectAttr "stripOrigShape.o" "deleteComponent1.ig";
connectAttr "deleteComponent1.og" "polySeparate1.ip";
connectAttr "polySeparate1.out[0]" "groupParts1.ig";
connectAttr "polySeparate1.out[1]" "groupParts2.ig";
connectAttr "groupParts1.og" "partAShape.i";
connectAttr "groupParts2.og" "partBShape.i";
connectAttr "strip2OrigShape.o" "polyChipOff1.ip";
connectAttr "polyChipOff1.out" "polySeparate2.ip";
connectAttr "polySeparate2.out[0]" "groupParts3.ig";
connectAttr "polySeparate2.out[1]" "groupParts4.ig";
connectAttr "groupParts3.og" "chipShape.i";
connectAttr "groupParts4.og" "restShape.i";
connectAttr "quadAShape.o" "polyUnite1.ip[0]";
connectAttr "quadBShape.o" "polyUnite1.ip[1]";
connectAttr "quadAShape.wm" "polyUnite1.im[0]";
connectAttr "quadBShape.wm" "polyUnite1.im[1]";
connectAttr "polyUnite1.out" "polyMergeVert1.ip";
connectAttr "polyMergeVert1.out" "joinedShape.i";
connectAttr "strip3OrigShape.o" "polySplitRing1.ip";
connectAttr "polySplitRing1.out" "ringedShape.i";
// End of history_replay.ma
