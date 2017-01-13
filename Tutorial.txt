How to linking FFmpeg in Visual Studio

1. Download FFmpeg pre-build: https://ffmpeg.zeranoe.com/builds/
Choose platform x86 or x64 to download (maybe both)
Shared: bin folder (*.dll)
Dev: include folder (*.h) and lib folder (*.lib)

2. Create Visual C++ project (Console application -> Empty project)

3. Choose platform x86 or x64 to test

4. Build project

5. Copy *.dll to debug/release folder (platform must be appropriate)

6. Project Properties
    -> VC++ Directories
        -> Include Directories -> Edit -> Point to FFmpeg's include folder
        -> Libraries Directories -> Edit -> Point to FFmpeg's lib folder

7. For C++
#include <iostream>

extern "C"
{
	#include <libavcodec\avcodec.h>
}

using namespace std;

void main()
{
	cout << avcodec_configuration() << endl;
	system("pause");
}

8. For C
// NOTE: Must be change method compile first
// Project Properties -> C/C++ -> Compile As -> Compile as C Code (/TC)

#include <libavcodec\avcodec.h>

void main()
{
	printf("%s\n", avcodec_configuration());
	system("pause");
}
