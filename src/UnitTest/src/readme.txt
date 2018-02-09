The dumbwebserver.cpp in this directory uses MakeCert.run exe checked into the this 
directory to create an SSL cert for the dumwebserver to act as a server of SSL 
requests. This utility is binplaced into the Target\distrib\%BUILDTYPE%\%BUILDTARGET%\unittest 
directory. Dumbwebserver accesses this utility with the ..\..\MakeCert.run path. 
All existing tests that link to dumbwebserver are based in a directory structure 
that looks like Target\distrib\%BUILDTYPE%\%BUILDTARGET%\unittest\footest\test 
such that ..\..\MakeCert.run works. If you are adding a new test that uses dumbwebserver, 
but don't follow this directory structure, make sure that you binplace MakeCert.run 
appropriately for your test.