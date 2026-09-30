#ifndef GPU_H
#define GPU_H

int gpuInit(void);
void gpuRenderLine(int y); /* draws ONE screen line into the picture */
int gpuFrame(void);        /* shows the finished picture; returns 0 if window closed */
void gpuQuit(void);

#endif