#pragma once
// The desktop OpenGL (compatibility profile) headers on either host; APIENTRY is the calling convention of GL functions.
#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>
#ifndef APIENTRY
#define APIENTRY
#endif
