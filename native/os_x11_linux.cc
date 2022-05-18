#include <unistd.h>
#include <memory>
#include <iostream>
#include <napi.h>
#include <proc/readproc.h>
#include <xcb/composite.h>
#include <atomic>
#include <thread>
#include "os.h"
#include "linux/x11.h"
#include "linux/shm.h"

using namespace priv_os_x11;

std::vector<TrackedWindow> trackedWindows;
std::thread windowThread;
std::atomic<bool> windowThreadExists(false);
std::atomic<bool> windowThreadShouldRun(false);

void WindowThread(xcb_window_t);

xcb_window_t* GetFrame(xcb_window_t win) {
	auto result = std::find_if(trackedWindows.begin(), trackedWindows.end(), [&win](TrackedWindow w){return w.window == win;});
	return result == trackedWindows.end() ? nullptr : &(result->frame);
}

void OSWindow::SetBounds(JSRectangle bounds) {
	ensureConnection();
	auto frame = GetFrame(this->handle);
	if (frame) {
		if (bounds.width > 0 && bounds.height > 0) {
			int32_t config[] = {
				bounds.x,
				bounds.y,
				bounds.width,
				bounds.height,
			};
			xcb_configure_window(connection, *frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, &config);
			int32_t childConfig[] = { 0, 0, bounds.width, bounds.height };
			xcb_configure_window(connection, this->handle, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, &childConfig);
		} else {
			int32_t config[] = {
				bounds.x,
				bounds.y,
			};
			xcb_configure_window(connection, *frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, &config);
		}
		xcb_flush(connection);
	}
}

JSRectangle OSWindow::GetBounds() {
	ensureConnection();
	xcb_query_tree_cookie_t cookie = xcb_query_tree(connection, this->handle);
	xcb_query_tree_reply_t* reply = xcb_query_tree_reply(connection, cookie, NULL);
	if (!reply) {
		return JSRectangle();
	}
	xcb_get_geometry_cookie_t cookie2 = xcb_get_geometry(connection, reply->parent);
	free(reply);
	xcb_get_geometry_reply_t* reply2 = xcb_get_geometry_reply(connection, cookie2, NULL);
	if (!reply2) {
		return JSRectangle();
	}
	auto result = JSRectangle(reply2->x, reply2->y, reply2->width, reply2->height);
	free(reply2);
	return result;
}

JSRectangle OSWindow::GetClientBounds() {
	ensureConnection();
	xcb_get_geometry_cookie_t cookie = xcb_get_geometry(connection, this->handle);
	xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(connection, cookie, NULL);
	if (!reply) {
		return JSRectangle();
	}

	auto x = reply->x;
	auto y = reply->y;
	auto width = reply->width;
	auto height = reply->height;
	auto window = this->handle;
	while (true) {
		xcb_query_tree_cookie_t cookieTree = xcb_query_tree(connection, window);
		xcb_query_tree_reply_t* replyTree = xcb_query_tree_reply(connection, cookieTree, NULL);
		if (replyTree == NULL || replyTree->parent == reply->root) {
			break;
		}
		window = replyTree->parent;
		free(reply);
		free(replyTree);

		cookie = xcb_get_geometry(connection, window);
		reply = xcb_get_geometry_reply(connection, cookie, NULL);
		if (reply == NULL) {
			break;
		}

		x += reply->x;
		y += reply->y;
	}
	free(reply);
	return JSRectangle(x, y, width, height);
}

bool OSWindow::IsValid() {
	if (!this->handle) {
		return false;
	}

	ensureConnection();
	xcb_get_geometry_cookie_t cookie = xcb_get_geometry_unchecked(connection, this->handle);
	std::unique_ptr<xcb_get_geometry_reply_t, decltype(&free)> reply { xcb_get_geometry_reply(connection, cookie, NULL), &free };
	return !!reply;
}

std::string OSWindow::GetTitle() {
	ensureConnection();
	xcb_get_property_cookie_t cookie = xcb_get_property_unchecked(connection, 0, this->handle, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 100);
	std::unique_ptr<xcb_get_property_reply_t, decltype(&free)> reply { xcb_get_property_reply(connection, cookie, NULL), &free };
	if (!reply) {
		return std::string();
	}

	char* title = reinterpret_cast<char*>(xcb_get_property_value(reply.get()));
	int length = xcb_get_property_value_length(reply.get());

	return std::string(title, length);
}

Napi::Value OSWindow::ToJS(Napi::Env env) {
	return Napi::BigInt::New(env, (uint64_t) this->handle);
}

bool OSWindow::operator==(const OSWindow& other) const {
	return this->handle == other.handle;
}

bool OSWindow::operator<(const OSWindow& other) const {
	return this->handle < other.handle;
}

OSWindow OSWindow::FromJsValue(const Napi::Value jsval) {
	auto handle = jsval.As<Napi::BigInt>();
	bool lossless;
	xcb_window_t handleint = handle.Uint64Value(&lossless);
	if (!lossless) {
		Napi::RangeError::New(jsval.Env(), "Invalid handle").ThrowAsJavaScriptException();
	}
	return OSWindow(handleint);
}

void GetRsHandlesRecursively(const xcb_window_t window, std::vector<OSWindow>* out, unsigned int* deepest, unsigned int depth = 0) {
	const uint32_t long_length = 4096;
	xcb_query_tree_cookie_t cookie = xcb_query_tree(connection, window);
	xcb_query_tree_reply_t* reply = xcb_query_tree_reply(connection, cookie, NULL);
	if (reply == NULL) {
		return;
	}

	xcb_window_t* children = xcb_query_tree_children(reply);

	for (auto i = 0; i < xcb_query_tree_children_length(reply); i++) {
		// First, check WM_CLASS for either "RuneScape" or "steam_app_1343400"
		xcb_window_t child = children[i];
		xcb_get_property_cookie_t cookieProp = xcb_get_property(connection, 0, child, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, long_length);
		xcb_get_property_reply_t* replyProp = xcb_get_property_reply(connection, cookieProp, NULL);
		if (replyProp != NULL) {
			auto len = xcb_get_property_value_length(replyProp);
			// if len == long_length then that means we didn't read the whole property, so discard.
			if (len > 0 && (uint32_t)len < long_length) {
				char buffer[long_length] = { 0 };
				memcpy(buffer, xcb_get_property_value(replyProp), len);
				// first is instance name, then class name - both null terminated. we want class name.
				const char* classname = buffer + strlen(buffer) + 1;
				if (strcmp(classname, "RuneScape") == 0 || strcmp(classname, "steam_app_1343400") == 0) {
					// Now, only take this if it's one of the deepest instances found so far
					if (depth > *deepest) {
						out->clear();
						out->push_back(child);
						*deepest = depth;
					} else if (depth == *deepest) {
						out->push_back(child);
					}
				}
			}
		}
		free(replyProp);
		GetRsHandlesRecursively(child, out, deepest, depth + 1);
	}

	free(reply);
}

std::vector<OSWindow> OSGetRsHandles() {
	ensureConnection();
	std::vector<OSWindow> out;
	unsigned int deepest = 0;
	GetRsHandlesRecursively(rootWindow, &out, &deepest);
	return out;
}

void OSSetWindowParent(OSWindow window, OSWindow parent) {
	ensureConnection();

	// If the parent handle is 0, we're supposed to detach, not attach
	if (parent.handle != 0) {
		// Query the tree and geometry of the game window
		xcb_query_tree_cookie_t cookieTree = xcb_query_tree(connection, parent.handle);
		xcb_query_tree_reply_t* tree = xcb_query_tree_reply(connection, cookieTree, NULL);
		xcb_get_geometry_cookie_t cookieGeometry = xcb_get_geometry(connection, window.handle);
		xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(connection, cookieGeometry, NULL);

		if (tree && tree->parent != tree->root) {
			// Generate an ID and track it
			xcb_window_t id = xcb_generate_id(connection);
			trackedWindows.push_back(TrackedWindow(window.handle, id));

			// Set OverrideRedirect on the electron window, and request events
			const uint32_t evalues[] = { 1 };
			xcb_change_window_attributes(connection, window.handle, XCB_CW_OVERRIDE_REDIRECT, evalues);

			// Create a new window, parented to the game's parent, with the game view's geometry and OverrideRedirect
			const uint32_t fvalues[] = { 1, 33554431 };
			xcb_create_window(connection, XCB_COPY_FROM_PARENT, id, tree->parent, 0, 0, geometry->width, geometry->height,
								0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, fvalues);

			// Map our new frame
			xcb_map_window(connection, id);

			// Parent the electron window to our frame
			xcb_reparent_window(connection, window.handle, id, 0, 0);

			std::cout << "native: created frame " << id << " for electron window " << window.handle << std::endl;

			// Start an event-handling thread if there isn't one already running
			if (!windowThreadExists.load()) {
				std::cout << "native: starting window thread" << std::endl;
				
				// The thread needs a graphics context, which it can share between all the windows in its purview:
				// "The graphics context can be used with any drawable that has the same root and depth as the specified drawable."
				const uint32_t gc = xcb_generate_id(connection);
				const uint32_t gvalues[] = { 0x80AAAAAA, 0x80FFFFFF };
				xcb_create_gc(connection, gc, id, XCB_GC_BACKGROUND | XCB_GC_FOREGROUND, gvalues);
				
				// And start the thread
				windowThreadShouldRun = true;
				windowThreadExists = true;
				windowThread = std::thread(WindowThread, gc);
			}

			xcb_flush(connection);
		}

		free(tree);
		free(geometry);
	} else {
		auto frame = GetFrame(window.handle);
		if (frame) {
			xcb_reparent_window(connection, window.handle, rootWindow, 0, 0);
			std::cout << "native: destroying frame " << *frame << std::endl;
			if (trackedWindows.size() == 1) {
				windowThreadShouldRun = false;
				xcb_destroy_window(connection, *frame);
				xcb_flush(connection);
				windowThread.join();
				trackedWindows.clear();
			} else {
				xcb_destroy_window(connection, *frame);
				xcb_flush(connection);
				trackedWindows.erase(
					std::remove_if(trackedWindows.begin(), trackedWindows.end(), [&window](TrackedWindow w){return w.window == window.handle;}),
					trackedWindows.end()
				);
			}
		}
	}
}

void OSCaptureDesktopMulti(OSWindow wnd, vector<CaptureRect> rects) {
	ensureConnection();
	XShmCapture acquirer(connection, rootWindow);
	//TODO double check and document desktop 0 special case
	auto offset = wnd.GetClientBounds();

	for (CaptureRect &rect : rects) {
		acquirer.copy(reinterpret_cast<char*>(rect.data), rect.size, rect.rect.x + offset.x, rect.rect.y + offset.y, rect.rect.width, rect.rect.height);
	}
}

void OSCaptureWindowMulti(OSWindow wnd, vector<CaptureRect> rects) {
	ensureConnection();
	xcb_composite_redirect_window(connection, wnd.handle, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
	xcb_pixmap_t pixId = xcb_generate_id(connection);
	xcb_composite_name_window_pixmap(connection, wnd.handle, pixId);

	xcb_get_geometry_cookie_t cookie = xcb_get_geometry(connection, pixId);
	xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(connection, cookie, NULL);
	if (!reply) {
		xcb_free_pixmap(connection, pixId);
		return;
	}

	XShmCapture acquirer(connection, pixId);

	for (CaptureRect &rect : rects) {
		acquirer.copy(reinterpret_cast<char*>(rect.data), rect.size, rect.rect.x, rect.rect.y, rect.rect.width, rect.rect.height);
	}

	free(reply);
	xcb_free_pixmap(connection, pixId);
}

void OSCaptureMulti(OSWindow wnd, CaptureMode mode, vector<CaptureRect> rects, Napi::Env env) {
	switch (mode) {
		case CaptureMode::Desktop: {
			OSCaptureDesktopMulti(wnd, rects);
			break;
		}
		case CaptureMode::Window:
			OSCaptureWindowMulti(wnd, rects);
			break;
		default:
			throw Napi::RangeError::New(env, "Capture mode not supported");
	}
}

OSWindow OSGetActiveWindow() {
	xcb_get_property_cookie_t cookie = xcb_ewmh_get_active_window(&ewmhConnection, 0);
	xcb_window_t window;
	if (xcb_ewmh_get_active_window_reply(&ewmhConnection, cookie, &window, NULL) == 0) {
		return OSWindow(0);
	}

	return OSWindow(window);
}
	

void OSNewWindowListener(OSWindow wnd, WindowEventType type, Napi::Function cb) {

}

void OSRemoveWindowListener(OSWindow wnd, WindowEventType type, Napi::Function cb) {

}

void WindowThread(uint32_t gc) {
	xcb_generic_event_t* event;
	while (windowThreadShouldRun.load()) {
		event = xcb_wait_for_event(connection);
		if (event) {
			auto type = event->response_type & ~0x80;
			switch (type) {
				case 0: {
					xcb_generic_error_t* error = (xcb_generic_error_t*)event;
					std::cout << "native: error: code " << (int)error->error_code << "; " << (int)error->major_code << "." << (int)error->minor_code << std::endl;
					break;
				}
				case XCB_EXPOSE: {
					//xcb_expose_event_t* expose = (xcb_expose_event_t*)event;
					break;
				}
				default:
					//std::cout << "native: got event type " << type << std::endl;
					break;
			}
			free(event);
		} else {
			std::cout << "native: window thread encountered an error" << std::endl;
			break;
		}
	}
	windowThreadExists = false;
	std::cout << "native: window thread exiting" << std::endl;
}
