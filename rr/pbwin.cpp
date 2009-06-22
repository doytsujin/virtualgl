/* Copyright (C)2004 Landmark Graphics Corporation
 * Copyright (C)2005, 2006 Sun Microsystems, Inc.
 * Copyright (C)2009 D. R. Commander
 *
 * This library is free software and may be redistributed and/or modified under
 * the terms of the wxWindows Library License, Version 3.1 or (at your option)
 * any later version.  The full license is in the LICENSE.txt file included
 * with this distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * wxWindows Library License for more details.
 */

#include "pbwin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef USEMEDIALIB
#include <mlib.h>
#endif
#if defined(sun)||defined(linux)
#include "rrsunray.h"
#endif
#include "glxvisual.h"

#define INFAKER
#include "tempctx.h"
#include "fakerconfig.h"

extern Display *_localdpy;
#ifdef USEGLP
extern GLPDevice _localdev;
#endif

#define checkgl(m) if(glerror()) _throw("Could not "m);

#define _isright(drawbuf) (drawbuf==GL_RIGHT || drawbuf==GL_FRONT_RIGHT \
	|| drawbuf==GL_BACK_RIGHT)
#define leye(buf) (buf==GL_BACK? GL_BACK_LEFT: \
	(buf==GL_FRONT? GL_FRONT_LEFT:buf))
#define reye(buf) (buf==GL_BACK? GL_BACK_RIGHT: \
	(buf==GL_FRONT? GL_FRONT_RIGHT:buf))

static inline int _drawingtoright(void)
{
	GLint drawbuf=GL_LEFT;
	_glGetIntegerv(GL_DRAW_BUFFER, &drawbuf);
	return _isright(drawbuf);
}

// Generic OpenGL error checker (0 = no errors)
static int glerror(void)
{
	int i, ret=0;
	i=glGetError();
	while(i!=GL_NO_ERROR)
	{
		ret=1;
		rrout.print("[VGL] ERROR: OpenGL error 0x%.4x\n", i);
		i=glGetError();
	}
	return ret;
}

#ifndef min
 #define min(a,b) ((a)<(b)?(a):(b))
#endif

Window create_window(Display *dpy, GLXFBConfig config, int w, int h)
{
	XVisualInfo *vis;
	Window win;
	XSetWindowAttributes wattrs;
	Colormap cmap;

	if((vis=_glXGetVisualFromFBConfig(dpy, config))==NULL) return 0;
	cmap=XCreateColormap(dpy, RootWindow(dpy, vis->screen), vis->visual,
		AllocNone);
	wattrs.background_pixel = 0;
	wattrs.border_pixel = 0;
	wattrs.colormap = cmap;
	wattrs.event_mask = ExposureMask | StructureNotifyMask;
	win = XCreateWindow(dpy, RootWindow(dpy, vis->screen), 0, 0, w, h, 1,
		vis->depth, InputOutput, vis->visual,
		CWBackPixel | CWBorderPixel | CWEventMask | CWColormap, &wattrs);
	XMapWindow(dpy, win);
	return win;
}

pbuffer::pbuffer(int w, int h, GLXFBConfig config)
{
	if(!config || w<1 || h<1) _throw("Invalid argument");

	_cleared=false;  _stereo=false;
	#if 0
	const char *glxext=NULL;
	glxext=_glXQueryExtensionsString(dpy, DefaultScreen(dpy));
	if(!glxext || !strstr(glxext, "GLX_SGIX_pbuffer"))
		_throw("Pbuffer extension not supported on rendering display");
	#endif

	int pbattribs[]={GLX_PBUFFER_WIDTH, 0, GLX_PBUFFER_HEIGHT, 0,
		GLX_PRESERVED_CONTENTS, True, None};

	_w=w;  _h=h;
	pbattribs[1]=w;  pbattribs[3]=h;
	#ifdef SUNOGL
	tempctx tc(_localdpy, 0, 0, 0);
	#endif
	if(fconfig.usewindow) _drawable=create_window(_localdpy, config, w, h);
	else _drawable=glXCreatePbuffer(_localdpy, config, pbattribs);
	if(__vglServerVisualAttrib(config, GLX_STEREO)) _stereo=true;
	if(!_drawable) _throw("Could not create Pbuffer");
}

pbuffer::~pbuffer(void)
{
	if(_drawable)
	{
		#ifdef SUNOGL
		tempctx tc(_localdpy, 0, 0, 0);
		#endif
		if(fconfig.usewindow) XDestroyWindow(_localdpy, _drawable);
		else glXDestroyPbuffer(_localdpy, _drawable);
	}
}

void pbuffer::clear(void)
{
	if(_cleared) return;
	_cleared=true;
	GLfloat params[4];
	_glGetFloatv(GL_COLOR_CLEAR_VALUE, params);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(params[0], params[1], params[2], params[3]);
}

void pbuffer::swap(void)
{
	if(!fconfig.glp) _glXSwapBuffers(_localdpy, _drawable);
	#ifdef USEGLP
	else if(__glPSwapBuffers) _glPSwapBuffers(_drawable);
	#endif
}


// This class encapsulates the Pbuffer, its most recent ancestor, and
// information specific to its corresponding X window

pbwin::pbwin(Display *windpy, Window win)
{
	if(!windpy || !win) _throw("Invalid argument");
	_eventdpy=NULL;
	_windpy=windpy;  _win=win;
	_force=false;
	_oldpb=_pb=NULL;  _neww=_newh=-1;
	_blitter=NULL;
	_rrdpy=_rrmoviedpy=NULL;
	_prof_rb.setname("Readback  ");
	_prof_gamma.setname("Gamma     ");
	_prof_anaglyph.setname("Anaglyph  ");
	_syncdpy=false;
	_dirty=false;
	_rdirty=false;
	_autotestframecount=0;
	_truecolor=true;
	fconfig.compress(_windpy);
	_sunrayhandle=NULL;
	_wmdelete=false;
	XWindowAttributes xwa;
	XGetWindowAttributes(windpy, win, &xwa);
	if(!(xwa.your_event_mask&StructureNotifyMask))
	{
		if(!(_eventdpy=XOpenDisplay(DisplayString(windpy))))
			_throw("Could not clone X display connection");
		XSelectInput(_eventdpy, win, StructureNotifyMask);
		if(fconfig.verbose)
			rrout.println("[VGL] Selecting structure notify events in window 0x%.8x",
				win);
	}
	if(xwa.depth<24 || xwa.visual->c_class!=TrueColor) _truecolor=false;
	_gammacorrectedvisuals=__vglHasGCVisuals(windpy, DefaultScreen(windpy));
	_stereovisual=__vglClientVisualAttrib(windpy, DefaultScreen(windpy),
		xwa.visual->visualid, GLX_STEREO);
}

pbwin::~pbwin(void)
{
	_mutex.lock(false);
	if(_pb) {delete _pb;  _pb=NULL;}
	if(_oldpb) {delete _oldpb;  _oldpb=NULL;}
	if(_blitter) {delete _blitter;  _blitter=NULL;}
	if(_rrdpy) {delete _rrdpy;  _rrdpy=NULL;}
	if(_rrmoviedpy) {delete _rrmoviedpy;  _rrmoviedpy=NULL;}
	#if defined(sun)||defined(linux)
	if(_sunrayhandle)
	{
		if(RRSunRayDestroy(_sunrayhandle)==-1 && fconfig.verbose)
			rrout.println("[VGL] WARNING: %s", RRSunRayGetError(_sunrayhandle));
	}
	#endif 
	if(_eventdpy) {XCloseDisplay(_eventdpy);  _eventdpy=NULL;}
	_mutex.unlock(false);
}

int pbwin::init(int w, int h, GLXFBConfig config)
{
	if(!config || w<1 || h<1) _throw("Invalid argument");

	rrcs::safelock l(_mutex);
	if(_wmdelete) _throw("Window has been deleted by window manager");
	if(_pb && _pb->width()==w && _pb->height()==h) return 0;
	if((_pb=new pbuffer(w, h, config))==NULL)
			_throw("Could not create Pbuffer");
	_config=config;
	_force=true;
	return 1;
}

// The resize doesn't actually occur until the next time updatedrawable() is
// called

void pbwin::resize(int w, int h)
{
	rrcs::safelock l(_mutex);
	if(_wmdelete) _throw("Window has been deleted by window manager");
	if(w==0 && _pb) w=_pb->width();
	if(h==0 && _pb) h=_pb->height();
	if(_pb && _pb->width()==w && _pb->height()==h)
	{
		_neww=_newh=-1;
		return;
	}
	_neww=w;  _newh=h;
}

void pbwin::clear(void)
{
	rrcs::safelock l(_mutex);
	if(_wmdelete) _throw("Window has been deleted by window manager");
	if(_pb) _pb->clear();
}

void pbwin::cleanup(void)
{
	rrcs::safelock l(_mutex);
	if(_wmdelete) _throw("Window has been deleted by window manager");
	if(_oldpb) {delete _oldpb;  _oldpb=NULL;}
}

void pbwin::initfromwindow(GLXFBConfig config)
{
	XSync(_windpy, False);
	XWindowAttributes xwa;
	XGetWindowAttributes(_windpy, _win, &xwa);
	init(xwa.width, xwa.height, config);
}

// Get the current Pbuffer drawable

GLXDrawable pbwin::getdrawable(void)
{
	GLXDrawable retval=0;
	rrcs::safelock l(_mutex);
	if(_wmdelete) _throw("Window has been deleted by window manager");
	retval=_pb->drawable();
	return retval;
}

void pbwin::checkresize(void)
{
	if(_eventdpy)
	{
		if(XPending(_eventdpy)>0)
		{
			XEvent event;
			_XNextEvent(_eventdpy, &event);
			if(event.type==ConfigureNotify && event.xconfigure.window==_win
				&& event.xconfigure.width>0 && event.xconfigure.height>0)
				resize(event.xconfigure.width, event.xconfigure.height);
		}
	}
}

// Get the current Pbuffer drawable, but resize the Pbuffer first if necessary

GLXDrawable pbwin::updatedrawable(void)
{
	GLXDrawable retval=0;
	rrcs::safelock l(_mutex);
	if(_wmdelete) _throw("Window has been deleted by window manager");
	if(_neww>0 && _newh>0)
	{
		pbuffer *oldpb=_pb;
		if(init(_neww, _newh, _config)) _oldpb=oldpb;
		_neww=_newh=-1;
	}
	retval=_pb->drawable();
	return retval;
}

Display *pbwin::getwindpy(void)
{
	return _windpy;
}

Window pbwin::getwin(void)
{
	return _win;
}

void pbwin::swapbuffers(void)
{
	rrcs::safelock l(_mutex);
	if(_wmdelete) _throw("Window has been deleted by window manager");
	if(_pb) _pb->swap();
}

void pbwin::wmdelete(void)
{
	rrcs::safelock l(_mutex);
	_wmdelete=true;
}

void pbwin::readback(GLint drawbuf, bool spoillast, bool sync)
{
	fconfig.reloadenv();
	bool dostereo=false;  int stereomode=fconfig.stereo;

	if(!fconfig.readback) return;

	rrcs::safelock l(_mutex);
	if(_wmdelete) _throw("Window has been deleted by window manager");

	_dirty=false;

	int compress=(int)fconfig.compress();
	if(sync) {compress=RRCOMP_PROXY;}

	if(stereo() && stereomode!=RRSTEREO_LEYE && stereomode!=RRSTEREO_REYE)
	{
		if(_drawingtoright() || _rdirty) dostereo=true;
		_rdirty=false;
		if(dostereo && _Trans[compress]!=RRTRANS_VGL && stereomode==RRSTEREO_QUADBUF)
		{
			static bool message=false;
			if(!message)
			{
				rrout.println("[VGL] NOTICE: Quad-buffered stereo requires the VGL image transport.");
				rrout.println("[VGL]    Using anaglyphic stereo instead.");
				message=true;
			}
			stereomode=RRSTEREO_REDCYAN;				
		}
		else if(dostereo && !_stereovisual && stereomode==RRSTEREO_QUADBUF)
		{
			static bool message2=false;
			if(!message2)
			{
				rrout.println("[VGL] NOTICE: Cannot use quad-buffered stereo because no stereo visuals are");
				rrout.println("[VGL]    available on the client.  Using anaglyphic stereo instead.");
				message2=true;
			}
			stereomode=RRSTEREO_REDCYAN;				
		}
	}

	if(!_truecolor) compress=RRCOMP_PROXY;

	bool sharerrdpy=false;
	if(fconfig.moviefile)
	{
		if(fconfig.mcompress==compress && fconfig.mqual==fconfig.qual
			&& fconfig.msubsamp==fconfig.subsamp && !fconfig.spoil)
			sharerrdpy=true;

		if(!sharerrdpy)
		{
			if(!_rrmoviedpy)
				errifnot(_rrmoviedpy=new rrdisplayclient(NULL, NULL, true));
			sendvgl(_rrmoviedpy, drawbuf, false, dostereo, RRSTEREO_QUADBUF,
				fconfig.mcompress, fconfig.mqual, fconfig.msubsamp, true);
		}
	}

	switch(compress)
	{
		case RRCOMP_PROXY:
			sendx11(drawbuf, spoillast, sync, dostereo, stereomode);
			break;

		case RRCOMP_JPEG:
		case RRCOMP_RGB:
			if(!_rrdpy) errifnot(_rrdpy=new rrdisplayclient(_windpy,
				fconfig.client? fconfig.client:DisplayString(_windpy)));
			_rrdpy->record(sharerrdpy);
			sendvgl(_rrdpy, drawbuf, spoillast, dostereo, stereomode,
				(int)compress, fconfig.qual, fconfig.subsamp, sharerrdpy);
			break;

		case RRCOMP_SRDPCM:
		case RRCOMP_SRRGB:
		case RRCOMP_SRYUV:
			if(sendsr(drawbuf, spoillast, dostereo,	stereomode)==-1)
			sendx11(drawbuf, spoillast, sync, dostereo, stereomode, true);
	}
}

int pbwin::sendsr(GLint drawbuf, bool spoillast, bool dostereo, int stereomode)
{
	rrframe f;
	int pbw=_pb->width(), pbh=_pb->height();
	unsigned char *bitmap=NULL;  int pitch, bottomup, format;
	static bool sroffwarned=false, sronwarned=true;

	if(!_sunrayhandle) _sunrayhandle=RRSunRayInit(_windpy, _win);
	if(!_sunrayhandle) _throw("Could not initialize Sun Ray plugin");
	if(spoillast && fconfig.spoil && !RRSunRayFrameReady(_sunrayhandle))
		return 0;
	if(!(bitmap=RRSunRayGetFrame(_sunrayhandle, pbw, pbh, &pitch, &format,
		&bottomup)))
	{
		const char *err=RRSunRayGetError(_sunrayhandle);
		if(err) _throw(err);
		else
		{
			if(!sroffwarned)
			{
				rrout.println("[VGL] NOTICE: Could not use the Sun Ray image transport, probably because");
				rrout.println("[VGL]    there is no network route between this server and your Sun Ray");
				rrout.println("[VGL]    client.  Temporarily switching to the X11 image transport.");
				sroffwarned=true;  sronwarned=false;
			}
			return -1;
		}
	}
	else
	{
		if(!sronwarned)
		{
			rrout.println("[VGL] NOTICE: Sun Ray image transport has been re-activated.");
			sronwarned=true;
		}
		sroffwarned=false;
	}
	f.init(bitmap, pbw, pitch, pbh, rrsunray_ps[format],
		(rrsunray_bgr[format]? RRBMP_BGR:0) |
		(rrsunray_afirst[format]? RRBMP_ALPHAFIRST:0) |
		(bottomup? RRBMP_BOTTOMUP:0));
	int glformat= (rrsunray_ps[format]==3? GL_RGB:GL_RGBA);
	#ifdef GL_BGR_EXT
	if(format==RRSUNRAY_BGR) glformat=GL_BGR_EXT;
	#endif
	#ifdef GL_BGRA_EXT
	if(format==RRSUNRAY_BGRA) glformat=GL_BGRA_EXT;
	#endif
	#ifdef GL_ABGR_EXT
	if(format==RRSUNRAY_ABGR) glformat=GL_ABGR_EXT;
	#endif
	if(dostereo && stereomode==RRSTEREO_REDCYAN) makeanaglyph(&f, drawbuf);
	else
	{
		GLint buf=drawbuf;
		if(stereomode==RRSTEREO_REYE) buf=reye(drawbuf);
		else if(stereomode==RRSTEREO_LEYE) buf=leye(drawbuf);
		readpixels(0, 0, pbw, pitch, pbh, glformat, rrsunray_ps[format], bitmap,
			buf, bottomup);
	}
	if(fconfig.logo) f.addlogo();
	if(RRSunRaySendFrame(_sunrayhandle, bitmap, pbw, pbh, pitch, format,
		bottomup)==-1) _throw(RRSunRayGetError(_sunrayhandle));
	return 0;
}

void pbwin::sendvgl(rrdisplayclient *rrdpy, GLint drawbuf, bool spoillast,
	bool dostereo, int stereomode, int compress, int qual, int subsamp,
	bool domovie)
{
	int pbw=_pb->width(), pbh=_pb->height();

	if(_sunrayhandle)
	{
		RRSunRayDestroy(_sunrayhandle);  _sunrayhandle=NULL;
	}

	if(spoillast && fconfig.spoil && !rrdpy->frameready() && !domovie)
		return;
	rrframe *b;
	int flags=RRBMP_BOTTOMUP, format=GL_RGB;
	#ifdef GL_BGR_EXT
	if(littleendian() && compress!=RRCOMP_RGB)
	{
		format=GL_BGR_EXT;  flags|=RRBMP_BGR;
	}
	#endif
	errifnot(b=rrdpy->getbitmap(pbw, pbh, 3, flags,
		dostereo && stereomode==RRSTEREO_QUADBUF, domovie? false:fconfig.spoil));
	if(dostereo && stereomode==RRSTEREO_REDCYAN) makeanaglyph(b, drawbuf);
	else
	{
		GLint buf=drawbuf;
		if(dostereo || stereomode==RRSTEREO_LEYE) buf=leye(drawbuf);
		if(stereomode==RRSTEREO_REYE) buf=reye(drawbuf);
		readpixels(0, 0, b->_h.framew, b->_pitch, b->_h.frameh, format,
			b->_pixelsize, b->_bits, buf, dostereo);
		if(dostereo && b->_rbits)
			readpixels(0, 0, b->_h.framew, b->_pitch, b->_h.frameh, format,
				b->_pixelsize, b->_rbits, reye(drawbuf), dostereo);
	}
	b->_h.winid=_win;
	b->_h.framew=b->_h.width;
	b->_h.frameh=b->_h.height;
	b->_h.x=0;
	b->_h.y=0;
	b->_h.qual=qual;
	b->_h.subsamp=subsamp;
	b->_h.compress=(unsigned char)compress;
	if(!_syncdpy) {XSync(_windpy, False);  _syncdpy=true;}
	if(fconfig.logo) b->addlogo();
	rrdpy->sendframe(b);
}

void pbwin::sendx11(GLint drawbuf, bool spoillast, bool sync, bool dostereo,
	int stereomode, bool srfallback)
{
	int pbw=_pb->width(), pbh=_pb->height();

	if(_sunrayhandle && !srfallback)
	{
		RRSunRayDestroy(_sunrayhandle);  _sunrayhandle=NULL;
	}
	rrfb *b;
	if(!_blitter) errifnot(_blitter=new rrblitter());
	if(spoillast && fconfig.spoil && !_blitter->frameready()) return;
	errifnot(b=_blitter->getbitmap(_windpy, _win, pbw, pbh, fconfig.spoil));
	b->_flags|=RRBMP_BOTTOMUP;
	if(dostereo && stereomode==RRSTEREO_REDCYAN) makeanaglyph(b, drawbuf);
	else
	{
		int format;
		unsigned char *bits=b->_bits;
		switch(b->_pixelsize)
		{
			case 1:  format=GL_COLOR_INDEX;  break;
			case 3:
				format=GL_RGB;
				#ifdef GL_BGR_EXT
				if(b->_flags&RRBMP_BGR) format=GL_BGR_EXT;
				#endif
				break;
			case 4:
				format=GL_RGBA;
				#ifdef GL_BGRA_EXT
				if(b->_flags&RRBMP_BGR && !(b->_flags&RRBMP_ALPHAFIRST))
					format=GL_BGRA_EXT;
				#endif
				if(b->_flags&RRBMP_BGR && b->_flags&RRBMP_ALPHAFIRST)
				{
					#ifdef GL_ABGR_EXT
					format=GL_ABGR_EXT;
					#elif defined(GL_BGRA_EXT)
					format=GL_BGRA_EXT;  bits=b->_bits+1;
					#endif
				}
				if(!(b->_flags&RRBMP_BGR) && b->_flags&RRBMP_ALPHAFIRST)
				{
					format=GL_RGBA;  bits=b->_bits+1;
				}
				break;
			default:
				_throw("Unsupported pixel format");
		}
		GLint buf=drawbuf;
		if(stereomode==RRSTEREO_REYE) buf=reye(drawbuf);
		else if(stereomode==RRSTEREO_LEYE) buf=leye(drawbuf);
		readpixels(0, 0, min(pbw, b->_h.framew), b->_pitch,
			min(pbh, b->_h.frameh), format, b->_pixelsize, bits, buf, true);
	}
	if(fconfig.logo) b->addlogo();
	_blitter->sendframe(b, sync);
}

void pbwin::makeanaglyph(rrframe *b, int drawbuf)
{
	_r.init(b->_h, 1, b->_flags, false);
	readpixels(0, 0, _r._h.framew, _r._pitch, _r._h.frameh, GL_RED,
		_r._pixelsize, _r._bits, leye(drawbuf), false);
	_g.init(b->_h, 1, b->_flags, false);
	readpixels(0, 0, _g._h.framew, _g._pitch, _g._h.frameh, GL_GREEN,
		_g._pixelsize, _g._bits, reye(drawbuf), false);
	_b.init(b->_h, 1, b->_flags, false);
	readpixels(0, 0, _b._h.framew, _b._pitch, _b._h.frameh, GL_BLUE,
		_b._pixelsize, _b._bits, reye(drawbuf), false);
	_prof_anaglyph.startframe();
	b->makeanaglyph(_r, _g, _b);
	_prof_anaglyph.endframe(b->_h.framew*b->_h.frameh, 0, 1);
}

void pbwin::readpixels(GLint x, GLint y, GLint w, GLint pitch, GLint h,
	GLenum format, int ps, GLubyte *bits, GLint buf, bool stereo)
{

	GLint readbuf=GL_BACK;
	_glGetIntegerv(GL_READ_BUFFER, &readbuf);

	tempctx tc(_localdpy, EXISTING_DRAWABLE, GetCurrentDrawable());

	glReadBuffer(buf);
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);

	if(pitch%8==0) glPixelStorei(GL_PACK_ALIGNMENT, 8);
	else if(pitch%4==0) glPixelStorei(GL_PACK_ALIGNMENT, 4);
	else if(pitch%2==0) glPixelStorei(GL_PACK_ALIGNMENT, 2);
	else if(pitch%1==0) glPixelStorei(GL_PACK_ALIGNMENT, 1);

	int e=glGetError();
	while(e!=GL_NO_ERROR) e=glGetError();  // Clear previous error
	_prof_rb.startframe();
	glReadPixels(x, y, w, h, format, GL_UNSIGNED_BYTE, bits);
	_prof_rb.endframe(w*h, 0, stereo? 0.5 : 1);
	checkgl("Read Pixels");

	// Gamma correction
	if((!_gammacorrectedvisuals || !fconfig.gamma.usesun())
		&& fconfig.gamma!=0.0 && fconfig.gamma!=1.0 && fconfig.gamma!=-1.0)
	{
		_prof_gamma.startframe();
		static bool first=true;
		#ifdef USEMEDIALIB
		if(first)
		{
			first=false;
			if(fconfig.verbose)
				rrout.println("[VGL] Using mediaLib gamma correction (correction factor=%f)\n",
					(double)fconfig.gamma);
		}
		mlib_image *image=NULL;
		if((image=mlib_ImageCreateStruct(MLIB_BYTE, ps, w, h, pitch, bits))!=NULL)
		{
			unsigned char *luts[4]={fconfig.gamma._lut, fconfig.gamma._lut,
				fconfig.gamma._lut, fconfig.gamma._lut};
			mlib_ImageLookUp_Inp(image, (const void **)luts);
			mlib_ImageDelete(image);
		}
		else
		{
		#endif
		if(first)
		{
			first=false;
			if(fconfig.verbose)
				rrout.println("[VGL] Using software gamma correction (correction factor=%f)\n",
					(double)fconfig.gamma);
		}
		unsigned short *ptr1, *ptr2=(unsigned short *)(&bits[pitch*h]);
		for(ptr1=(unsigned short *)bits; ptr1<ptr2; ptr1++)
			*ptr1=fconfig.gamma._lut16[*ptr1];
		if((pitch*h)%2!=0) bits[pitch*h-1]=fconfig.gamma._lut[bits[pitch*h-1]];
		#ifdef USEMEDIALIB
		}
		#endif
		_prof_gamma.endframe(w*h, 0, stereo?0.5 : 1);
	}

	// If automatic faker testing is enabled, store the FB color in an
	// environment variable so the test program can verify it
	if(fconfig.autotest)
	{
		unsigned char *rowptr, *pixel;  int match=1;
		int color=-1, i, j, k;
		color=-1;
		if(buf!=GL_FRONT_RIGHT && buf!=GL_BACK_RIGHT) _autotestframecount++;
		for(j=0, rowptr=bits; j<h && match; j++, rowptr+=pitch)
			for(i=1, pixel=&rowptr[ps]; i<w && match; i++, pixel+=ps)
				for(k=0; k<ps; k++)
				{
					if(pixel[k]!=rowptr[k]) {match=0;  break;}
				}
		if(match)
		{
			if(format==GL_COLOR_INDEX)
			{
				unsigned char index;
				glReadPixels(0, 0, 1, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, &index);
				color=index;
			}
			else
			{
				unsigned char rgb[3];
				glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
				color=rgb[0]+(rgb[1]<<8)+(rgb[2]<<16);
			}
		}
		if(buf==GL_FRONT_RIGHT || buf==GL_BACK_RIGHT)
		{
			snprintf(_autotestrclr, 79, "__VGL_AUTOTESTRCLR%x=%d", (unsigned int)_win, color);
			putenv(_autotestrclr);
		}
		else
		{
			snprintf(_autotestclr, 79, "__VGL_AUTOTESTCLR%x=%d", (unsigned int)_win, color);
			putenv(_autotestclr);
		}
		snprintf(_autotestframe, 79, "__VGL_AUTOTESTFRAME%x=%d", (unsigned int)_win, _autotestframecount);
		putenv(_autotestframe);
	}

	glPopClientAttrib();
	tc.restore();
	glReadBuffer(readbuf);
}

bool pbwin::stereo(void)
{
	return (_pb && _pb->stereo());
}
