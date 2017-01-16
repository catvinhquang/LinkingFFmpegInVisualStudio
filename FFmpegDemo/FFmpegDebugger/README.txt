*.LIB, *.H, *.DLL in this project isn't same version in FFmpegDemo project

FFmpeg commands:
1. Trim without reencode:
-i ./input.mp4 -ss 00:00:10 -t 00:00:30 -c copy ./output.mp4

2. Trim with reencode:
-i ./input.mp4 -ss 00:00:10 -t 00:00:30 ./output.mp4

3. Scale:
-i ./input.mp4 -vf scale=360:640 ./output.mp4

4. Trim and Scale:
-i ./input.mp4 -vf scale=360:640 -ss 00:00:10 -t 00:00:30 ./output.mp4

5. Speed up (timelapse)
-i ./input.mp4 -filter:v "setpts=0.5*PTS" -y ./output.mp4

6. Speed down (slow motion)
-i ./input.mp4 -filter:v "setpts=1.5*PTS" -y ./output.mp4

7. Reverse
-i ./input.mp4 -vf reverse ./output.mp4