/*

Copyright (C) 2010 Hans-Werner Hilse <hilse@web.de>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <jni.h>

#include <android/log.h>

#include <fitz.h>
#include <mupdf.h>


/************************************************************************/
/* Macros: */

/* Bytes per pixel */

#define BYPP 4

/* Debugging helper */

#define DEBUG(args...) \
	__android_log_print(ANDROID_LOG_DEBUG, "PdfRender", args)
//#define DEBUG(args...) {}
#define ERROR(args...) \
	__android_log_print(ANDROID_LOG_ERROR, "PdfRender", args)
#define INFO(args...) \
	__android_log_print(ANDROID_LOG_INFO, "PdfRender", args)

/* Exception classes */

#define EXC						"java/lang/Exception"
#define EXC_CANNOT_REPAIR		"java/lang/Exception"
#define EXC_CANNOT_DECRYPTXREF	"java/lang/Exception"
#define EXC_NEED_PASSWORD		"java/lang/Exception"
#define EXC_PAGELOAD			"java/lang/Exception"
#define EXC_PAGERENDER			"java/lang/Exception"
#define EXC_WRONG_PASSWORD		"java/lang/Exception"


/************************************************************************/

typedef struct renderdocument_s renderdocument_t;
struct renderdocument_s
{
	pdf_xref *xref;
	pdf_outline *outline;
	fz_renderer *rast;
};

typedef struct renderpage_s renderpage_t;
struct renderpage_s
{
	pdf_page *page;
	fz_matrix ctm;
	fz_rect bbox;
};

/************************************************************************/

/* our own helper functions: */

void throw_exception(JNIEnv *env, char *exception_class, char *message)
{
	jthrowable new_exception = (*env)->FindClass(env, exception_class);
	if(new_exception == NULL) {
		ERROR("cannot create Exception '%s', Message was '%s'");
		return;
	}
	(*env)->ThrowNew(env, new_exception, message);
}

/* JNI Interface: */

JNI_OnLoad(JavaVM *jvm, void *reserved)
{
	DEBUG("initializing PdfRender JNI library based on MuPDF");

	/* Fitz library setup */
	fz_cpudetect();
	fz_accelerate();

	return JNI_VERSION_1_2;
}

JNIEXPORT jlong JNICALL
	Java_de_hilses_droidreader_PdfDocument_nativeOpen
	(JNIEnv *env, jobject this,
			jint fitzmemory, jstring fname, jstring pwd)
{
	DEBUG("PdfDocument(%p).nativeOpen(%i, \"%p\", \"%p\")",
			this, fitzmemory, fname, pwd);

	fz_error error;
	fz_obj *obj;
	renderdocument_t *doc;
	jboolean iscopy;
	jclass cls;
	jfieldID fid;
	char *filename;
	char *password;

	filename = (*env)->GetStringUTFChars(env, fname, &iscopy);
	password = (*env)->GetStringUTFChars(env, pwd, &iscopy);

	doc = fz_malloc(sizeof(renderdocument_t));
	if(!doc) {
		throw_exception(env, EXC, "Out of Memory");
		goto cleanup;
	}

	/* initialize renderer */

	error = fz_newrenderer(&doc->rast, pdf_devicergb, 0, (int) fitzmemory);
	if (error) {
		throw_exception(env, EXC, "Cannot create new renderer");
		goto cleanup;
	}

	/*
	 * Open PDF and load xref table
	 */

	doc->xref = pdf_newxref();
	error = pdf_loadxref(doc->xref, filename);
	if (error) {
		/* TODO: plug into fitz error handling */
		fz_catch(error, "trying to repair");
		INFO("Corrupted file '%s', trying to repair", filename);
		error = pdf_repairxref(doc->xref, filename);
		if (error) {
			throw_exception(env, EXC_CANNOT_REPAIR,
					"PDF file is corrupted");
			goto cleanup;
		}
	}

	error = pdf_decryptxref(doc->xref);
	if (error) {
		throw_exception(env, EXC_CANNOT_DECRYPTXREF,
				"Cannot decrypt XRef table");
		goto cleanup;
	}

	/*
	 * Handle encrypted PDF files
	 */

	if (pdf_needspassword(doc->xref)) {
		if(strlen(password)) {
			error = pdf_authenticatepassword(doc->xref, password);
			if(error) {
				throw_exception(env, EXC_WRONG_PASSWORD,
						"Wrong password given");
				goto cleanup;
			}
		} else {
			throw_exception(env, EXC_NEED_PASSWORD,
					"PDF needs a password!");
			goto cleanup;
		}
	}

	/*
	 * Load document metadata (at some point this might be implemented
	 * in the muPDF lib itself)
	 */

	obj = fz_dictgets(doc->xref->trailer, "Root");
	doc->xref->root = fz_resolveindirect(obj);
	if (!doc->xref->root) {
		fz_throw("syntaxerror: missing Root object");
		throw_exception(env, EXC, "PDF syntax: missing \"Root\" object");
		goto cleanup;
	}
	fz_keepobj(doc->xref->root);

	obj = fz_dictgets(doc->xref->trailer, "Info");
	doc->xref->info = fz_resolveindirect(obj);
	if (doc->xref->info)
		fz_keepobj(doc->xref->info);

	cls = (*env)->GetObjectClass(env, this);

	doc->outline = pdf_loadoutline(doc->xref);
	/* TODO: passing outline to Java env or create accessor functions */

	if (doc->xref->info) {
		obj = fz_dictgets(doc->xref->info, "Title");
		if (obj) {
			fid = (*env)->GetFieldID(env, cls, "metaTitle",
					"Ljava/lang/String;");
			if(fid) {
				jstring jstr = (*env)->NewStringUTF(env, pdf_toutf8(obj));
				(*env)->SetObjectField(env, this, fid, jstr);
			}
		}
	}

	fid = (*env)->GetFieldID(env, cls, "pagecount","I");
	if(fid) {
		(*env)->SetIntField(env, this, fid,
				pdf_getpagecount(doc->xref));
	} else {
		throw_exception(env, EXC, "cannot access instance fields!");
	}

cleanup:

	(*env)->ReleaseStringUTFChars(env, fname, filename);
	(*env)->ReleaseStringUTFChars(env, pwd, password);

	DEBUG("PdfDocument.nativeOpen(): return handle = %p", doc);
	return (jlong) doc;
}

JNIEXPORT jlong JNICALL
	Java_de_hilses_droidreader_PdfDocument_nativeClose
	(JNIEnv *env, jobject this, jlong handle)
{
	renderdocument_t *doc = (renderdocument_t*) handle;
	DEBUG("PdfDocument(%p).nativeClose(%p)", this, doc);

	if(doc) {
		if (doc->outline)
			pdf_dropoutline(doc->outline);

		if (doc->xref->store)
			pdf_dropstore(doc->xref->store);

		pdf_closexref(doc->xref);

		fz_droprenderer(doc->rast);

		fz_free(doc);
	}
	DEBUG("PdfDocument.nativeClose(): return handle = %p", doc);
	return (jlong) doc;
}

JNIEXPORT jlong JNICALL
	Java_de_hilses_droidreader_PdfPage_nativeOpenPage
	(JNIEnv *env, jobject this, jlong dochandle, jfloatArray mediabox, jint pageno)
{
	renderdocument_t *doc = (renderdocument_t*) dochandle;
	DEBUG("PdfPage(%p).nativeOpenPage(%p)", this, doc);
	renderpage_t *page;
	fz_error error;
	fz_obj *obj;
	jclass cls;
	jfieldID fid;

	page = fz_malloc(sizeof(renderpage_t));
	if(!page) {
		throw_exception(env, EXC, "Out of Memory");
		return (jlong) NULL;
	}

	pdf_flushxref(doc->xref, 0);
	obj = pdf_getpageobject(doc->xref, pageno);
	error = pdf_loadpage(&page->page, doc->xref, obj);
	if (error) {
		throw_exception(env, EXC_PAGELOAD, "error loading page");
		goto cleanup;
	}

	jfloat *bbox = (*env)->GetPrimitiveArrayCritical(env, mediabox, 0);
	if(bbox == NULL) {
		throw_exception(env, EXC, "out of memory");
		goto cleanup;
	}
	bbox[0] = page->page->mediabox.x0;
	bbox[1] = page->page->mediabox.y0;
	bbox[2] = page->page->mediabox.x1;
	bbox[3] = page->page->mediabox.y1;
	(*env)->ReleasePrimitiveArrayCritical(env, mediabox, bbox, 0);

	cls = (*env)->GetObjectClass(env, this);
	fid = (*env)->GetFieldID(env, cls, "rotate","I");
	if(fid) {
		(*env)->SetIntField(env, this, fid, page->page->rotate);
	} else {
		throw_exception(env, EXC, "cannot access instance fields!");
	}

cleanup:
	/* nothing yet */

	DEBUG("PdfPage.nativeOpenPage(): return handle = %p", page);
	return (jlong) page;
}

JNIEXPORT long JNICALL
	Java_de_hilses_droidreader_PdfPage_nativeClosePage
	(JNIEnv *env, jobject this, jlong handle)
{
	renderpage_t *page = (renderpage_t*) handle;
	DEBUG("PdfPage(%p).nativeClosePage(%p)", this, page);
	if(page) {
		if (page->page)
			pdf_droppage(page->page);

		fz_free(page);
	}
	DEBUG("PdfPage.nativeClosePage(): return handle = %p", page);
	return (jlong) page;
}

JNIEXPORT void JNICALL
	Java_de_hilses_droidreader_PdfView_nativeCreateView
	(JNIEnv *env, jobject this, jlong dochandle, jlong pagehandle,
		jintArray viewboxarray, jfloatArray matrixarray,
		jintArray bufferarray)
{
	renderdocument_t *doc = (renderdocument_t*) dochandle;
	renderpage_t *page = (renderpage_t*) pagehandle;
	DEBUG("PdfView(%p).nativeCreateView(%p, %p)", this, doc, page);
	fz_error error;
	fz_matrix ctm;
	fz_irect viewbox;
	fz_pixmap *pixmap;
	jfloat *matrix;
	jint *viewboxarr;
	jint *dimen;
	jint *buffer;
	int length, val;

	/* initialize parameter arrays for MuPDF */

	matrix = (*env)->GetPrimitiveArrayCritical(env, matrixarray, 0);
	ctm.a = matrix[0];
	ctm.b = matrix[1];
	ctm.c = matrix[2];
	ctm.d = matrix[3];
	ctm.e = matrix[4];
	ctm.f = matrix[5];
	(*env)->ReleasePrimitiveArrayCritical(env, matrixarray, matrix, 0);
	DEBUG("Matrix: %f %f %f %f %f %f",
			ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);

	viewboxarr = (*env)->GetPrimitiveArrayCritical(env, viewboxarray, 0);
	viewbox.x0 = viewboxarr[0];
	viewbox.y0 = viewboxarr[1];
	viewbox.x1 = viewboxarr[2];
	viewbox.y1 = viewboxarr[3];
	(*env)->ReleasePrimitiveArrayCritical(env, viewboxarray, viewboxarr, 0);
	DEBUG("Viewbox: %d %d %d %d",
			viewbox.x0, viewbox.y0, viewbox.x1, viewbox.y1);

	/* do the rendering */
	DEBUG("doing the rendering...");
	buffer = (*env)->GetPrimitiveArrayCritical(env, bufferarray, 0);

	error = fz_newpixmapwithbufferandrect(&pixmap, (void*)buffer, viewbox, 4);
	if(!error)
		error = fz_rendertreetopixmap(&pixmap, doc->rast,
				page->page->tree, ctm,
				viewbox, 1);

	/* evil magic: we transform the rendered image's byte order
	 */
	if(!error) {
		DEBUG("Converting image buffer pixel order");
		length = pixmap->w * pixmap->h;
		unsigned int *col = pixmap->samples;
		int c = 0;
		for(val = 0; val < length; val++) {
			col[val] = ((col[val] & 0xFF000000) >> 24) |
					((col[val] & 0x00FF0000) >> 8) |
					((col[val] & 0x0000FF00) << 8);
		}
	}

	(*env)->ReleasePrimitiveArrayCritical(env, bufferarray, buffer, 0);

	fz_droppixmapwithoutbuffer(pixmap);

	if (error) {
		DEBUG("error!");
		throw_exception(env, EXC_PAGERENDER, "error rendering page");
	}

	DEBUG("PdfView.nativeCreateView() done");
}
