/*************************************************************************/
/*  script_editor_debugger.cpp                                           */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2020 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2020 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "script_editor_debugger.h"

#include "core/debugger/debugger_marshalls.h"
#include "core/io/marshalls.h"
#include "core/project_settings.h"
#include "core/ustring.h"
#include "editor/debugger/editor_network_profiler.h"
#include "editor/debugger/editor_profiler.h"
#include "editor/debugger/editor_visual_profiler.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/editor_scale.h"
#include "editor/editor_settings.h"
#include "editor/plugins/canvas_item_editor_plugin.h"
#include "editor/plugins/node_3d_editor_plugin.h"
#include "editor/property_editor.h"
#include "main/performance.h"
#include "scene/3d/camera_3d.h"
#include "scene/debugger/scene_debugger.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/texture_button.h"
#include "scene/gui/tree.h"
#include "scene/resources/packed_scene.h"
#include "servers/display_server.h"

using CameraOverride = EditorDebuggerNode::CameraOverride;

void ScriptEditorDebugger::_put_msg(String p_message, Array p_data) {
	if (is_session_active()) {
		Array msg;
		msg.push_back(p_message);
		msg.push_back(p_data);
		peer->put_message(msg);
	}
}

void ScriptEditorDebugger::debug_copy() {
	String msg = reason->get_text();
	if (msg == "") return;
	DisplayServer::get_singleton()->clipboard_set(msg);
}

void ScriptEditorDebugger::debug_skip_breakpoints() {
	skip_breakpoints_value = !skip_breakpoints_value;
	if (skip_breakpoints_value)
		skip_breakpoints->set_icon(get_theme_icon("DebugSkipBreakpointsOn", "EditorIcons"));
	else
		skip_breakpoints->set_icon(get_theme_icon("DebugSkipBreakpointsOff", "EditorIcons"));

	Array msg;
	msg.push_back(skip_breakpoints_value);
	_put_msg("set_skip_breakpoints", msg);
}

void ScriptEditorDebugger::debug_next() {

	ERR_FAIL_COND(!breaked);

	_put_msg("next", Array());
	_clear_execution();
}
void ScriptEditorDebugger::debug_step() {

	ERR_FAIL_COND(!breaked);

	_put_msg("step", Array());
	_clear_execution();
}

void ScriptEditorDebugger::debug_break() {

	ERR_FAIL_COND(breaked);

	_put_msg("break", Array());
}

void ScriptEditorDebugger::debug_continue() {

	ERR_FAIL_COND(!breaked);

	// Allow focus stealing only if we actually run this client for security.
	if (remote_pid && EditorNode::get_singleton()->has_child_process(remote_pid))
		DisplayServer::get_singleton()->enable_for_stealing_focus(remote_pid);

	_clear_execution();
	_put_msg("continue", Array());
}

void ScriptEditorDebugger::update_tabs() {
	if (error_count == 0 && warning_count == 0) {
		errors_tab->set_name(TTR("Errors"));
		tabs->set_tab_icon(errors_tab->get_index(), Ref<Texture2D>());
	} else {
		errors_tab->set_name(TTR("Errors") + " (" + itos(error_count + warning_count) + ")");
		if (error_count == 0) {
			tabs->set_tab_icon(errors_tab->get_index(), get_theme_icon("Warning", "EditorIcons"));
		} else {
			tabs->set_tab_icon(errors_tab->get_index(), get_theme_icon("Error", "EditorIcons"));
		}
	}
}

void ScriptEditorDebugger::clear_style() {
	tabs->add_theme_style_override("panel", NULL);
}

void ScriptEditorDebugger::save_node(ObjectID p_id, const String &p_file) {
	Array msg;
	msg.push_back(p_id);
	msg.push_back(p_file);
	_put_msg("scene:save_node", msg);
}

void ScriptEditorDebugger::_file_selected(const String &p_file) {
	Error err;
	FileAccessRef file = FileAccess::open(p_file, FileAccess::WRITE, &err);

	if (err != OK) {
		ERR_PRINT("Failed to open " + p_file);
		return;
	}
	Vector<String> line;
	line.resize(Performance::MONITOR_MAX);

	// signatures
	for (int i = 0; i < Performance::MONITOR_MAX; i++) {
		line.write[i] = Performance::get_singleton()->get_monitor_name(Performance::Monitor(i));
	}
	file->store_csv_line(line);

	// values
	List<Vector<float>>::Element *E = perf_history.back();
	while (E) {

		Vector<float> &perf_data = E->get();
		for (int i = 0; i < perf_data.size(); i++) {

			line.write[i] = String::num_real(perf_data[i]);
		}
		file->store_csv_line(line);
		E = E->prev();
	}
	file->store_string("\n");

	Vector<Vector<String>> profiler_data = profiler->get_data_as_csv();
	for (int i = 0; i < profiler_data.size(); i++) {
		file->store_csv_line(profiler_data[i]);
	}
}

void ScriptEditorDebugger::request_remote_tree() {

	_put_msg("scene:request_scene_tree", Array());
}

const SceneDebuggerTree *ScriptEditorDebugger::get_remote_tree() {
	return scene_tree;
}

void ScriptEditorDebugger::update_remote_object(ObjectID p_obj_id, const String &p_prop, const Variant &p_value) {

	Array msg;
	msg.push_back(p_obj_id);
	msg.push_back(p_prop);
	msg.push_back(p_value);
	_put_msg("scene:set_object_property", msg);
}

void ScriptEditorDebugger::request_remote_object(ObjectID p_obj_id) {

	ERR_FAIL_COND(p_obj_id.is_null());
	Array msg;
	msg.push_back(p_obj_id);
	_put_msg("scene:inspect_object", msg);
}

Object *ScriptEditorDebugger::get_remote_object(ObjectID p_id) {
	return inspector->get_object(p_id);
}

void ScriptEditorDebugger::_remote_object_selected(ObjectID p_id) {
	emit_signal("remote_object_requested", p_id);
}

void ScriptEditorDebugger::_remote_object_edited(ObjectID p_id, const String &p_prop, const Variant &p_value) {
	update_remote_object(p_id, p_prop, p_value);
	request_remote_object(p_id);
}

void ScriptEditorDebugger::_remote_object_property_updated(ObjectID p_id, const String &p_property) {
	emit_signal("remote_object_property_updated", p_id, p_property);
}

void ScriptEditorDebugger::_video_mem_request() {

	_put_msg("core:memory", Array());
}

Size2 ScriptEditorDebugger::get_minimum_size() const {

	Size2 ms = MarginContainer::get_minimum_size();
	ms.y = MAX(ms.y, 250 * EDSCALE);
	return ms;
}

void ScriptEditorDebugger::_parse_message(const String &p_msg, const Array &p_data) {

	if (p_msg == "debug_enter") {

		_put_msg("get_stack_dump", Array());

		ERR_FAIL_COND(p_data.size() != 2);
		bool can_continue = p_data[0];
		String error = p_data[1];
		breaked = true;
		can_debug = can_continue;
		_update_buttons_state();
		_set_reason_text(error, MESSAGE_ERROR);
		emit_signal("breaked", true, can_continue);
		DisplayServer::get_singleton()->window_move_to_foreground();
		if (error != "") {
			tabs->set_current_tab(0);
		}
		profiler->set_enabled(false);
		inspector->clear_cache(); // Take a chance to force remote objects update.

	} else if (p_msg == "debug_exit") {

		breaked = false;
		can_debug = false;
		_clear_execution();
		_update_buttons_state();
		_set_reason_text(TTR("Execution resumed."), MESSAGE_SUCCESS);
		emit_signal("breaked", false, false);
		profiler->set_enabled(true);
		profiler->disable_seeking();
	} else if (p_msg == "set_pid") {

		ERR_FAIL_COND(p_data.size() < 1);
		remote_pid = p_data[0];
	} else if (p_msg == "scene:click_ctrl") {

		ERR_FAIL_COND(p_data.size() < 2);
		clicked_ctrl->set_text(p_data[0]);
		clicked_ctrl_type->set_text(p_data[1]);
	} else if (p_msg == "scene:scene_tree") {

		scene_tree->nodes.clear();
		scene_tree->deserialize(p_data);
		emit_signal("remote_tree_updated");
		_update_buttons_state();
	} else if (p_msg == "scene:inspect_object") {

		ObjectID id = inspector->add_object(p_data);
		if (id.is_valid())
			emit_signal("remote_object_updated", id);
	} else if (p_msg == "memory:usage") {

		vmem_tree->clear();
		TreeItem *root = vmem_tree->create_item();
		DebuggerMarshalls::ResourceUsage usage;
		usage.deserialize(p_data);

		int total = 0;

		for (List<DebuggerMarshalls::ResourceInfo>::Element *E = usage.infos.front(); E; E = E->next()) {

			TreeItem *it = vmem_tree->create_item(root);
			String type = E->get().type;
			int bytes = E->get().vram;
			it->set_text(0, E->get().path);
			it->set_text(1, type);
			it->set_text(2, E->get().format);
			it->set_text(3, String::humanize_size(bytes));
			total += bytes;

			if (has_theme_icon(type, "EditorIcons"))
				it->set_icon(0, get_theme_icon(type, "EditorIcons"));
		}

		vmem_total->set_tooltip(TTR("Bytes:") + " " + itos(total));
		vmem_total->set_text(String::humanize_size(total));

	} else if (p_msg == "stack_dump") {

		DebuggerMarshalls::ScriptStackDump stack;
		stack.deserialize(p_data);

		stack_dump->clear();
		inspector->clear_stack_variables();
		TreeItem *r = stack_dump->create_item();

		for (int i = 0; i < stack.frames.size(); i++) {

			TreeItem *s = stack_dump->create_item(r);
			Dictionary d;
			d["frame"] = i;
			d["file"] = stack.frames[i].file;
			d["function"] = stack.frames[i].func;
			d["line"] = stack.frames[i].line;
			s->set_metadata(0, d);

			String line = itos(i) + " - " + String(d["file"]) + ":" + itos(d["line"]) + " - at function: " + d["function"];
			s->set_text(0, line);

			if (i == 0)
				s->select(0);
		}
	} else if (p_msg == "stack_frame_vars") {

		inspector->clear_stack_variables();

	} else if (p_msg == "stack_frame_var") {

		inspector->add_stack_variable(p_data);

	} else if (p_msg == "output") {
		ERR_FAIL_COND(p_data.size() < 1);
		ERR_FAIL_COND(p_data[0].get_type() != Variant::PACKED_STRING_ARRAY);
		Vector<String> strings = p_data[0];
		EditorNode::get_log()->add_message(String("\n").join(strings));
	} else if (p_msg == "performance:profile_frame") {
		Vector<float> p;
		p.resize(p_data.size());
		for (int i = 0; i < p_data.size(); i++) {
			p.write[i] = p_data[i];
			if (i < perf_items.size()) {

				const float value = p[i];
				String label = rtos(value);
				String tooltip = label;
				switch (Performance::MonitorType((int)perf_items[i]->get_metadata(1))) {
					case Performance::MONITOR_TYPE_MEMORY: {
						label = String::humanize_size(value);
						tooltip = label;
					} break;
					case Performance::MONITOR_TYPE_TIME: {
						label = rtos(value * 1000).pad_decimals(2) + " ms";
						tooltip = label;
					} break;
					default: {
						tooltip += " " + perf_items[i]->get_text(0);
					} break;
				}

				perf_items[i]->set_text(1, label);
				perf_items[i]->set_tooltip(1, tooltip);
				if (p[i] > perf_max[i])
					perf_max.write[i] = p[i];
			}
		}
		perf_history.push_front(p);
		perf_draw->update();

	} else if (p_msg == "visual:profile_frame") {
		DebuggerMarshalls::VisualProfilerFrame frame;
		frame.deserialize(p_data);

		EditorVisualProfiler::Metric metric;
		metric.areas.resize(frame.areas.size());
		metric.frame_number = frame.frame_number;
		metric.valid = true;

		{
			EditorVisualProfiler::Metric::Area *areas_ptr = metric.areas.ptrw();
			for (int i = 0; i < frame.areas.size(); i++) {
				areas_ptr[i].name = frame.areas[i].name;
				areas_ptr[i].cpu_time = frame.areas[i].cpu_msec;
				areas_ptr[i].gpu_time = frame.areas[i].gpu_msec;
			}
		}
		visual_profiler->add_frame_metric(metric);

	} else if (p_msg == "error") {

		DebuggerMarshalls::OutputError oe;
		ERR_FAIL_COND_MSG(oe.deserialize(p_data) == false, "Failed to deserialize error message");

		// Format time.
		Array time_vals;
		time_vals.push_back(oe.hr);
		time_vals.push_back(oe.min);
		time_vals.push_back(oe.sec);
		time_vals.push_back(oe.msec);
		bool e;
		String time = String("%d:%02d:%02d:%04d").sprintf(time_vals, &e);

		// Rest of the error data.
		bool source_is_project_file = oe.source_file.begins_with("res://");

		// Metadata to highlight error line in scripts.
		Array source_meta;
		source_meta.push_back(oe.source_file);
		source_meta.push_back(oe.source_line);

		// Create error tree to display above error or warning details.
		TreeItem *r = error_tree->get_root();
		if (!r) {
			r = error_tree->create_item();
		}

		// Also provide the relevant details as tooltip to quickly check without
		// uncollapsing the tree.
		String tooltip = oe.warning ? TTR("Warning:") : TTR("Error:");

		TreeItem *error = error_tree->create_item(r);
		error->set_collapsed(true);

		error->set_icon(0, get_theme_icon(oe.warning ? "Warning" : "Error", "EditorIcons"));
		error->set_text(0, time);
		error->set_text_align(0, TreeItem::ALIGN_LEFT);

		String error_title;
		// Include method name, when given, in error title.
		if (!oe.source_func.empty())
			error_title += oe.source_func + ": ";
		// If we have a (custom) error message, use it as title, and add a C++ Error
		// item with the original error condition.
		error_title += oe.error_descr.empty() ? oe.error : oe.error_descr;
		error->set_text(1, error_title);
		tooltip += " " + error_title + "\n";

		if (!oe.error_descr.empty()) {
			// Add item for C++ error condition.
			TreeItem *cpp_cond = error_tree->create_item(error);
			cpp_cond->set_text(0, "<" + TTR("C++ Error") + ">");
			cpp_cond->set_text(1, oe.error);
			cpp_cond->set_text_align(0, TreeItem::ALIGN_LEFT);
			tooltip += TTR("C++ Error:") + " " + oe.error + "\n";
			if (source_is_project_file)
				cpp_cond->set_metadata(0, source_meta);
		}
		Vector<uint8_t> v;
		v.resize(100);

		// Source of the error.
		String source_txt = (source_is_project_file ? oe.source_file.get_file() : oe.source_file) + ":" + itos(oe.source_line);
		if (!oe.source_func.empty())
			source_txt += " @ " + oe.source_func + "()";

		TreeItem *cpp_source = error_tree->create_item(error);
		cpp_source->set_text(0, "<" + (source_is_project_file ? TTR("Source") : TTR("C++ Source")) + ">");
		cpp_source->set_text(1, source_txt);
		cpp_source->set_text_align(0, TreeItem::ALIGN_LEFT);
		tooltip += (source_is_project_file ? TTR("Source:") : TTR("C++ Source:")) + " " + source_txt + "\n";

		// Set metadata to highlight error line in scripts.
		if (source_is_project_file) {
			error->set_metadata(0, source_meta);
			cpp_source->set_metadata(0, source_meta);
		}

		error->set_tooltip(0, tooltip);
		error->set_tooltip(1, tooltip);

		// Format stack trace.
		// stack_items_count is the number of elements to parse, with 3 items per frame
		// of the stack trace (script, method, line).
		const ScriptLanguage::StackInfo *infos = oe.callstack.ptr();
		for (unsigned int i = 0; i < (unsigned int)oe.callstack.size(); i++) {

			TreeItem *stack_trace = error_tree->create_item(error);

			Array meta;
			meta.push_back(infos[i].file);
			meta.push_back(infos[i].line);
			stack_trace->set_metadata(0, meta);

			if (i == 0) {
				stack_trace->set_text(0, "<" + TTR("Stack Trace") + ">");
				stack_trace->set_text_align(0, TreeItem::ALIGN_LEFT);
				error->set_metadata(0, meta);
			}
			stack_trace->set_text(1, infos[i].file.get_file() + ":" + itos(infos[i].line) + " @ " + infos[i].func + "()");
		}

		if (oe.warning)
			warning_count++;
		else
			error_count++;

	} else if (p_msg == "servers:function_signature") {
		// Cache a profiler signature.
		DebuggerMarshalls::ScriptFunctionSignature sig;
		sig.deserialize(p_data);
		profiler_signature[sig.id] = sig.name;

	} else if (p_msg == "servers:profile_frame" || p_msg == "servers:profile_total") {
		EditorProfiler::Metric metric;
		DebuggerMarshalls::ServersProfilerFrame frame;
		frame.deserialize(p_data);
		metric.valid = true;
		metric.frame_number = frame.frame_number;
		metric.frame_time = frame.frame_time;
		metric.idle_time = frame.idle_time;
		metric.physics_time = frame.physics_time;
		metric.physics_frame_time = frame.physics_frame_time;

		if (frame.servers.size()) {
			EditorProfiler::Metric::Category frame_time;
			frame_time.signature = "category_frame_time";
			frame_time.name = "Frame Time";
			frame_time.total_time = metric.frame_time;

			EditorProfiler::Metric::Category::Item item;
			item.calls = 1;
			item.line = 0;

			item.name = "Physics Time";
			item.total = metric.physics_time;
			item.self = item.total;
			item.signature = "physics_time";

			frame_time.items.push_back(item);

			item.name = "Idle Time";
			item.total = metric.idle_time;
			item.self = item.total;
			item.signature = "idle_time";

			frame_time.items.push_back(item);

			item.name = "Physics Frame Time";
			item.total = metric.physics_frame_time;
			item.self = item.total;
			item.signature = "physics_frame_time";

			frame_time.items.push_back(item);

			metric.categories.push_back(frame_time);
		}

		for (int i = 0; i < frame.servers.size(); i++) {

			const DebuggerMarshalls::ServerInfo &srv = frame.servers[i];
			EditorProfiler::Metric::Category c;
			const String name = srv.name;
			c.name = name.capitalize();
			c.items.resize(srv.functions.size());
			c.total_time = 0;
			c.signature = "categ::" + name;
			for (int j = 0; j < srv.functions.size(); j++) {

				EditorProfiler::Metric::Category::Item item;
				item.calls = 1;
				item.line = 0;
				item.name = srv.functions[j].name;
				item.self = srv.functions[j].time;
				item.total = item.self;
				item.signature = "categ::" + name + "::" + item.name;
				item.name = item.name.capitalize();
				c.total_time += item.total;
				c.items.write[j] = item;
			}
			metric.categories.push_back(c);
		}

		EditorProfiler::Metric::Category funcs;
		funcs.total_time = frame.script_time;
		funcs.items.resize(frame.script_functions.size());
		funcs.name = "Script Functions";
		funcs.signature = "script_functions";
		for (int i = 0; i < frame.script_functions.size(); i++) {

			int signature = frame.script_functions[i].sig_id;
			int calls = frame.script_functions[i].call_count;
			float total = frame.script_functions[i].total_time;
			float self = frame.script_functions[i].self_time;

			EditorProfiler::Metric::Category::Item item;
			if (profiler_signature.has(signature)) {

				item.signature = profiler_signature[signature];

				String name = profiler_signature[signature];
				Vector<String> strings = name.split("::");
				if (strings.size() == 3) {
					item.name = strings[2];
					item.script = strings[0];
					item.line = strings[1].to_int();
				} else if (strings.size() == 4) { //Built-in scripts have an :: in their name
					item.name = strings[3];
					item.script = strings[0] + "::" + strings[1];
					item.line = strings[2].to_int();
				}

			} else {
				item.name = "SigErr " + itos(signature);
			}

			item.calls = calls;
			item.self = self;
			item.total = total;
			funcs.items.write[i] = item;
		}

		metric.categories.push_back(funcs);

		if (p_msg == "servers:profile_frame")
			profiler->add_frame_metric(metric, false);
		else
			profiler->add_frame_metric(metric, true);

	} else if (p_msg == "network:profile_frame") {
		DebuggerMarshalls::NetworkProfilerFrame frame;
		frame.deserialize(p_data);
		for (int i = 0; i < frame.infos.size(); i++) {
			network_profiler->add_node_frame_data(frame.infos[i]);
		}

	} else if (p_msg == "network:bandwidth") {
		ERR_FAIL_COND(p_data.size() < 2);
		network_profiler->set_bandwidth(p_data[0], p_data[1]);

	} else if (p_msg == "request_quit") {
		emit_signal("stop_requested");
		_stop_and_notify();

	} else {
		WARN_PRINT("unknown message " + p_msg);
	}
}

void ScriptEditorDebugger::_set_reason_text(const String &p_reason, MessageType p_type) {
	switch (p_type) {
		case MESSAGE_ERROR:
			reason->add_theme_color_override("font_color", get_theme_color("error_color", "Editor"));
			break;
		case MESSAGE_WARNING:
			reason->add_theme_color_override("font_color", get_theme_color("warning_color", "Editor"));
			break;
		default:
			reason->add_theme_color_override("font_color", get_theme_color("success_color", "Editor"));
	}
	reason->set_text(p_reason);
	reason->set_tooltip(p_reason.word_wrap(80));
}

void ScriptEditorDebugger::_performance_select() {

	perf_draw->update();
}

void ScriptEditorDebugger::_performance_draw() {

	Vector<int> which;
	for (int i = 0; i < perf_items.size(); i++) {

		if (perf_items[i]->is_checked(0))
			which.push_back(i);
	}

	if (which.empty()) {
		info_message->show();
		return;
	}

	info_message->hide();

	Ref<StyleBox> graph_sb = get_theme_stylebox("normal", "TextEdit");
	Ref<Font> graph_font = get_theme_font("font", "TextEdit");

	int cols = Math::ceil(Math::sqrt((float)which.size()));
	int rows = Math::ceil((float)which.size() / cols);
	if (which.size() == 1)
		rows = 1;

	int margin = 3;
	int point_sep = 5;
	Size2i s = Size2i(perf_draw->get_size()) / Size2i(cols, rows);
	for (int i = 0; i < which.size(); i++) {

		Point2i p(i % cols, i / cols);
		Rect2i r(p * s, s);
		r.position += Point2(margin, margin);
		r.size -= Point2(margin, margin) * 2.0;
		perf_draw->draw_style_box(graph_sb, r);
		r.position += graph_sb->get_offset();
		r.size -= graph_sb->get_minimum_size();
		int pi = which[i];
		Color c = get_theme_color("accent_color", "Editor");
		float h = (float)which[i] / (float)(perf_items.size());
		// Use a darker color on light backgrounds for better visibility
		float value_multiplier = EditorSettings::get_singleton()->is_dark_theme() ? 1.4 : 0.55;
		c.set_hsv(Math::fmod(h + 0.4, 0.9), c.get_s() * 0.9, c.get_v() * value_multiplier);

		c.a = 0.6;
		perf_draw->draw_string(graph_font, r.position + Point2(0, graph_font->get_ascent()), perf_items[pi]->get_text(0), c, r.size.x);
		c.a = 0.9;
		perf_draw->draw_string(graph_font, r.position + Point2(0, graph_font->get_ascent() + graph_font->get_height()), perf_items[pi]->get_text(1), c, r.size.y);

		float spacing = point_sep / float(cols);
		float from = r.size.width;

		List<Vector<float>>::Element *E = perf_history.front();
		float prev = -1;
		while (from >= 0 && E) {

			float m = perf_max[pi];
			if (m == 0)
				m = 0.00001;
			float h2 = E->get()[pi] / m;
			h2 = (1.0 - h2) * r.size.y;

			if (E != perf_history.front())
				perf_draw->draw_line(r.position + Point2(from, h2), r.position + Point2(from + spacing, prev), c, Math::round(EDSCALE));
			prev = h2;
			E = E->next();
			from -= spacing;
		}
	}
}

void ScriptEditorDebugger::_notification(int p_what) {

	switch (p_what) {

		case NOTIFICATION_ENTER_TREE: {

			skip_breakpoints->set_icon(get_theme_icon("DebugSkipBreakpointsOff", "EditorIcons"));
			copy->set_icon(get_theme_icon("ActionCopy", "EditorIcons"));

			step->set_icon(get_theme_icon("DebugStep", "EditorIcons"));
			next->set_icon(get_theme_icon("DebugNext", "EditorIcons"));
			dobreak->set_icon(get_theme_icon("Pause", "EditorIcons"));
			docontinue->set_icon(get_theme_icon("DebugContinue", "EditorIcons"));
			le_set->connect("pressed", callable_mp(this, &ScriptEditorDebugger::_live_edit_set));
			le_clear->connect("pressed", callable_mp(this, &ScriptEditorDebugger::_live_edit_clear));
			error_tree->connect("item_selected", callable_mp(this, &ScriptEditorDebugger::_error_selected));
			error_tree->connect("item_activated", callable_mp(this, &ScriptEditorDebugger::_error_activated));
			vmem_refresh->set_icon(get_theme_icon("Reload", "EditorIcons"));

			reason->add_theme_color_override("font_color", get_theme_color("error_color", "Editor"));

		} break;
		case NOTIFICATION_PROCESS: {

			if (is_session_active()) {

				if (camera_override == CameraOverride::OVERRIDE_2D) {
					CanvasItemEditor *editor = CanvasItemEditor::get_singleton();

					Dictionary state = editor->get_state();
					float zoom = state["zoom"];
					Point2 offset = state["ofs"];
					Transform2D transform;

					transform.scale_basis(Size2(zoom, zoom));
					transform.elements[2] = -offset * zoom;

					Array msg;
					msg.push_back(transform);
					_put_msg("scene:override_camera_2D:transform", msg);

				} else if (camera_override >= CameraOverride::OVERRIDE_3D_1) {
					int viewport_idx = camera_override - CameraOverride::OVERRIDE_3D_1;
					Node3DEditorViewport *viewport = Node3DEditor::get_singleton()->get_editor_viewport(viewport_idx);
					Camera3D *const cam = viewport->get_camera();

					Array msg;
					msg.push_back(cam->get_camera_transform());
					if (cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
						msg.push_back(false);
						msg.push_back(cam->get_size());
					} else {
						msg.push_back(true);
						msg.push_back(cam->get_fov());
					}
					msg.push_back(cam->get_znear());
					msg.push_back(cam->get_zfar());
					_put_msg("scene:override_camera_3D:transform", msg);
				}
			}

			const uint64_t until = OS::get_singleton()->get_ticks_msec() + 20;

			while (peer.is_valid() && peer->has_message()) {

				Array arr = peer->get_message();
				if (arr.size() != 2 || arr[0].get_type() != Variant::STRING || arr[1].get_type() != Variant::ARRAY) {
					_stop_and_notify();
					ERR_FAIL_MSG("Invalid message format received from peer");
				}
				_parse_message(arr[0], arr[1]);

				if (OS::get_singleton()->get_ticks_msec() > until)
					break;
			}
			if (!is_session_active()) {
				_stop_and_notify();
				break;
			};
		} break;
		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {

			if (tabs->has_theme_stylebox_override("panel")) {
				tabs->add_theme_style_override("panel", editor->get_gui_base()->get_theme_stylebox("DebuggerPanel", "EditorStyles"));
			}

			copy->set_icon(get_theme_icon("ActionCopy", "EditorIcons"));
			step->set_icon(get_theme_icon("DebugStep", "EditorIcons"));
			next->set_icon(get_theme_icon("DebugNext", "EditorIcons"));
			dobreak->set_icon(get_theme_icon("Pause", "EditorIcons"));
			docontinue->set_icon(get_theme_icon("DebugContinue", "EditorIcons"));
			vmem_refresh->set_icon(get_theme_icon("Reload", "EditorIcons"));
		} break;
	}
}

void ScriptEditorDebugger::_clear_execution() {
	TreeItem *ti = stack_dump->get_selected();
	if (!ti)
		return;

	Dictionary d = ti->get_metadata(0);

	stack_script = ResourceLoader::load(d["file"]);
	emit_signal("clear_execution", stack_script);
	stack_script.unref();
	stack_dump->clear();
	inspector->clear_stack_variables();
}

void ScriptEditorDebugger::start(Ref<RemoteDebuggerPeer> p_peer) {

	error_count = 0;
	warning_count = 0;
	stop();

	peer = p_peer;
	ERR_FAIL_COND(p_peer.is_null());

	perf_history.clear();
	for (int i = 0; i < Performance::MONITOR_MAX; i++) {

		perf_max.write[i] = 0;
	}

	set_process(true);
	breaked = false;
	can_debug = true;
	camera_override = CameraOverride::OVERRIDE_NONE;

	tabs->set_current_tab(0);
	_set_reason_text(TTR("Debug session started."), MESSAGE_SUCCESS);
	_update_buttons_state();
}

void ScriptEditorDebugger::_update_buttons_state() {
	const bool active = is_session_active();
	const bool has_editor_tree = active && editor_remote_tree && editor_remote_tree->get_selected();
	vmem_refresh->set_disabled(!active);
	step->set_disabled(!active || !breaked || !can_debug);
	next->set_disabled(!active || !breaked || !can_debug);
	copy->set_disabled(!active || !breaked);
	docontinue->set_disabled(!active || !breaked);
	dobreak->set_disabled(!active || breaked);
	le_clear->set_disabled(!active);
	le_set->set_disabled(!has_editor_tree);
}

void ScriptEditorDebugger::_stop_and_notify() {
	stop();
	emit_signal("stopped");
	_set_reason_text(TTR("Debug session closed."), MESSAGE_WARNING);
}

void ScriptEditorDebugger::stop() {

	set_process(false);
	breaked = false;
	can_debug = false;
	remote_pid = 0;
	_clear_execution();

	inspector->clear_cache();

	if (peer.is_valid()) {
		peer->close();
		peer.unref();
		reason->set_text("");
		reason->set_tooltip("");
	}

	node_path_cache.clear();
	res_path_cache.clear();
	profiler_signature.clear();

	inspector->edit(NULL);
	_update_buttons_state();
}

void ScriptEditorDebugger::_profiler_activate(bool p_enable, int p_type) {

	Array data;
	data.push_back(p_enable);
	switch (p_type) {
		case PROFILER_NETWORK:
			_put_msg("profiler:network", data);
			break;
		case PROFILER_VISUAL:
			_put_msg("profiler:visual", data);
			break;
		case PROFILER_SCRIPTS_SERVERS:
			if (p_enable) {
				// Clear old script signatures. (should we move all this into the profiler?)
				profiler_signature.clear();
				// Add max funcs options to request.
				Array opts;
				int max_funcs = EditorSettings::get_singleton()->get("debugger/profiler_frame_max_functions");
				opts.push_back(CLAMP(max_funcs, 16, 512));
				data.push_back(opts);
			}
			_put_msg("profiler:servers", data);
			break;
		default:
			ERR_FAIL_MSG("Invalid profiler type");
	}
}

void ScriptEditorDebugger::_profiler_seeked() {

	if (breaked)
		return;
	debug_break();
}

void ScriptEditorDebugger::_stack_dump_frame_selected() {

	emit_signal("stack_frame_selected");

	int frame = get_stack_script_frame();

	if (is_session_active() && frame >= 0) {
		Array msg;
		msg.push_back(frame);
		_put_msg("get_stack_frame_vars", msg);
	} else {
		inspector->edit(NULL);
	}
}

void ScriptEditorDebugger::_export_csv() {

	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	file_dialog->popup_centered_ratio();
}

String ScriptEditorDebugger::get_var_value(const String &p_var) const {
	if (!breaked)
		return String();
	return inspector->get_stack_variable(p_var);
}

int ScriptEditorDebugger::_get_node_path_cache(const NodePath &p_path) {

	const int *r = node_path_cache.getptr(p_path);
	if (r)
		return *r;

	last_path_id++;

	node_path_cache[p_path] = last_path_id;
	Array msg;
	msg.push_back(p_path);
	msg.push_back(last_path_id);
	_put_msg("scene:live_node_path", msg);

	return last_path_id;
}

int ScriptEditorDebugger::_get_res_path_cache(const String &p_path) {

	Map<String, int>::Element *E = res_path_cache.find(p_path);

	if (E)
		return E->get();

	last_path_id++;

	res_path_cache[p_path] = last_path_id;
	Array msg;
	msg.push_back(p_path);
	msg.push_back(last_path_id);
	_put_msg("scene:live_res_path", msg);

	return last_path_id;
}

void ScriptEditorDebugger::_method_changed(Object *p_base, const StringName &p_name, VARIANT_ARG_DECLARE) {

	if (!p_base || !live_debug || !is_session_active() || !editor->get_edited_scene())
		return;

	Node *node = Object::cast_to<Node>(p_base);

	VARIANT_ARGPTRS

	for (int i = 0; i < VARIANT_ARG_MAX; i++) {
		//no pointers, sorry
		if (argptr[i] && (argptr[i]->get_type() == Variant::OBJECT || argptr[i]->get_type() == Variant::_RID))
			return;
	}

	if (node) {

		NodePath path = editor->get_edited_scene()->get_path_to(node);
		int pathid = _get_node_path_cache(path);

		Array msg;
		msg.push_back(pathid);
		msg.push_back(p_name);
		for (int i = 0; i < VARIANT_ARG_MAX; i++) {
			//no pointers, sorry
			msg.push_back(*argptr[i]);
		}
		_put_msg("scene:live_node_call", msg);

		return;
	}

	Resource *res = Object::cast_to<Resource>(p_base);

	if (res && res->get_path() != String()) {

		String respath = res->get_path();
		int pathid = _get_res_path_cache(respath);

		Array msg;
		msg.push_back(pathid);
		msg.push_back(p_name);
		for (int i = 0; i < VARIANT_ARG_MAX; i++) {
			//no pointers, sorry
			msg.push_back(*argptr[i]);
		}
		_put_msg("scene:live_res_call", msg);

		return;
	}
}

void ScriptEditorDebugger::_property_changed(Object *p_base, const StringName &p_property, const Variant &p_value) {

	if (!p_base || !live_debug || !editor->get_edited_scene())
		return;

	Node *node = Object::cast_to<Node>(p_base);

	if (node) {

		NodePath path = editor->get_edited_scene()->get_path_to(node);
		int pathid = _get_node_path_cache(path);

		if (p_value.is_ref()) {
			Ref<Resource> res = p_value;
			if (res.is_valid() && res->get_path() != String()) {

				Array msg;
				msg.push_back(pathid);
				msg.push_back(p_property);
				msg.push_back(res->get_path());
				_put_msg("scene:live_node_prop_res", msg);
			}
		} else {

			Array msg;
			msg.push_back(pathid);
			msg.push_back(p_property);
			msg.push_back(p_value);
			_put_msg("scene:live_node_prop", msg);
		}

		return;
	}

	Resource *res = Object::cast_to<Resource>(p_base);

	if (res && res->get_path() != String()) {

		String respath = res->get_path();
		int pathid = _get_res_path_cache(respath);

		if (p_value.is_ref()) {
			Ref<Resource> res2 = p_value;
			if (res2.is_valid() && res2->get_path() != String()) {

				Array msg;
				msg.push_back(pathid);
				msg.push_back(p_property);
				msg.push_back(res2->get_path());
				_put_msg("scene:live_res_prop_res", msg);
			}
		} else {

			Array msg;
			msg.push_back(pathid);
			msg.push_back(p_property);
			msg.push_back(p_value);
			_put_msg("scene:live_res_prop", msg);
		}

		return;
	}
}

String ScriptEditorDebugger::get_stack_script_file() const {
	TreeItem *ti = stack_dump->get_selected();
	if (!ti)
		return "";
	Dictionary d = ti->get_metadata(0);
	return d["file"];
}

int ScriptEditorDebugger::get_stack_script_line() const {
	TreeItem *ti = stack_dump->get_selected();
	if (!ti)
		return -1;
	Dictionary d = ti->get_metadata(0);
	return d["line"];
}

int ScriptEditorDebugger::get_stack_script_frame() const {
	TreeItem *ti = stack_dump->get_selected();
	if (!ti)
		return -1;
	Dictionary d = ti->get_metadata(0);
	return d["frame"];
}

void ScriptEditorDebugger::set_live_debugging(bool p_enable) {

	live_debug = p_enable;
}

void ScriptEditorDebugger::_live_edit_set() {

	if (!is_session_active() || !editor_remote_tree)
		return;

	TreeItem *ti = editor_remote_tree->get_selected();
	if (!ti)
		return;

	String path;

	while (ti) {
		String lp = ti->get_text(0);
		path = "/" + lp + path;
		ti = ti->get_parent();
	}

	NodePath np = path;

	editor->get_editor_data().set_edited_scene_live_edit_root(np);

	update_live_edit_root();
}

void ScriptEditorDebugger::_live_edit_clear() {

	NodePath np = NodePath("/root");
	editor->get_editor_data().set_edited_scene_live_edit_root(np);

	update_live_edit_root();
}

void ScriptEditorDebugger::update_live_edit_root() {

	NodePath np = editor->get_editor_data().get_edited_scene_live_edit_root();

	Array msg;
	msg.push_back(np);
	if (editor->get_edited_scene())
		msg.push_back(editor->get_edited_scene()->get_filename());
	else
		msg.push_back("");
	_put_msg("scene:live_set_root", msg);
	live_edit_root->set_text(np);
}

void ScriptEditorDebugger::live_debug_create_node(const NodePath &p_parent, const String &p_type, const String &p_name) {

	if (live_debug) {
		Array msg;
		msg.push_back(p_parent);
		msg.push_back(p_type);
		msg.push_back(p_name);
		_put_msg("scene:live_create_node", msg);
	}
}

void ScriptEditorDebugger::live_debug_instance_node(const NodePath &p_parent, const String &p_path, const String &p_name) {

	if (live_debug) {
		Array msg;
		msg.push_back(p_parent);
		msg.push_back(p_path);
		msg.push_back(p_name);
		_put_msg("scene:live_instance_node", msg);
	}
}
void ScriptEditorDebugger::live_debug_remove_node(const NodePath &p_at) {

	if (live_debug) {
		Array msg;
		msg.push_back(p_at);
		_put_msg("scene:live_remove_node", msg);
	}
}
void ScriptEditorDebugger::live_debug_remove_and_keep_node(const NodePath &p_at, ObjectID p_keep_id) {

	if (live_debug) {
		Array msg;
		msg.push_back(p_at);
		msg.push_back(p_keep_id);
		_put_msg("scene:live_remove_and_keep_node", msg);
	}
}
void ScriptEditorDebugger::live_debug_restore_node(ObjectID p_id, const NodePath &p_at, int p_at_pos) {

	if (live_debug) {
		Array msg;
		msg.push_back(p_id);
		msg.push_back(p_at);
		msg.push_back(p_at_pos);
		_put_msg("scene:live_restore_node", msg);
	}
}
void ScriptEditorDebugger::live_debug_duplicate_node(const NodePath &p_at, const String &p_new_name) {

	if (live_debug) {
		Array msg;
		msg.push_back(p_at);
		msg.push_back(p_new_name);
		_put_msg("scene:live_duplicate_node", msg);
	}
}
void ScriptEditorDebugger::live_debug_reparent_node(const NodePath &p_at, const NodePath &p_new_place, const String &p_new_name, int p_at_pos) {

	if (live_debug) {
		Array msg;
		msg.push_back(p_at);
		msg.push_back(p_new_place);
		msg.push_back(p_new_name);
		msg.push_back(p_at_pos);
		_put_msg("scene:live_reparent_node", msg);
	}
}

CameraOverride ScriptEditorDebugger::get_camera_override() const {
	return camera_override;
}

void ScriptEditorDebugger::set_camera_override(CameraOverride p_override) {

	if (p_override == CameraOverride::OVERRIDE_2D && camera_override != CameraOverride::OVERRIDE_2D) {
		Array msg;
		msg.push_back(true);
		_put_msg("scene:override_camera_2D:set", msg);
	} else if (p_override != CameraOverride::OVERRIDE_2D && camera_override == CameraOverride::OVERRIDE_2D) {
		Array msg;
		msg.push_back(false);
		_put_msg("scene:override_camera_2D:set", msg);
	} else if (p_override >= CameraOverride::OVERRIDE_3D_1 && camera_override < CameraOverride::OVERRIDE_3D_1) {
		Array msg;
		msg.push_back(true);
		_put_msg("scene:override_camera_3D:set", msg);
	} else if (p_override < CameraOverride::OVERRIDE_3D_1 && camera_override >= CameraOverride::OVERRIDE_3D_1) {
		Array msg;
		msg.push_back(false);
		_put_msg("scene:override_camera_3D:set", msg);
	}

	camera_override = p_override;
}

void ScriptEditorDebugger::set_breakpoint(const String &p_path, int p_line, bool p_enabled) {

	Array msg;
	msg.push_back(p_path);
	msg.push_back(p_line);
	msg.push_back(p_enabled);
	_put_msg("breakpoint", msg);
}

void ScriptEditorDebugger::reload_scripts() {

	_put_msg("reload_scripts", Array());
}

bool ScriptEditorDebugger::is_skip_breakpoints() {
	return skip_breakpoints_value;
}

void ScriptEditorDebugger::_error_activated() {
	TreeItem *selected = error_tree->get_selected();

	TreeItem *ci = selected->get_children();
	if (ci) {
		selected->set_collapsed(!selected->is_collapsed());
	}
}

void ScriptEditorDebugger::_error_selected() {
	TreeItem *selected = error_tree->get_selected();
	Array meta = selected->get_metadata(0);
	if (meta.size() == 0) {
		return;
	}

	emit_signal("error_selected", String(meta[0]), int(meta[1]));
}

void ScriptEditorDebugger::_expand_errors_list() {

	TreeItem *root = error_tree->get_root();
	if (!root)
		return;

	TreeItem *item = root->get_children();
	while (item) {
		item->set_collapsed(false);
		item = item->get_next();
	}
}

void ScriptEditorDebugger::_collapse_errors_list() {

	TreeItem *root = error_tree->get_root();
	if (!root)
		return;

	TreeItem *item = root->get_children();
	while (item) {
		item->set_collapsed(true);
		item = item->get_next();
	}
}

void ScriptEditorDebugger::_clear_errors_list() {

	error_tree->clear();
	error_count = 0;
	warning_count = 0;
}

// Right click on specific file(s) or folder(s).
void ScriptEditorDebugger::_error_tree_item_rmb_selected(const Vector2 &p_pos) {

	item_menu->clear();
	item_menu->set_size(Size2(1, 1));

	if (error_tree->is_anything_selected()) {
		item_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), TTR("Copy Error"), 0);
	}

	if (item_menu->get_item_count() > 0) {
		item_menu->set_position(error_tree->get_global_position() + p_pos);
		item_menu->popup();
	}
}

void ScriptEditorDebugger::_item_menu_id_pressed(int p_option) {
	TreeItem *ti = error_tree->get_selected();
	while (ti->get_parent() != error_tree->get_root())
		ti = ti->get_parent();

	String type;

	if (ti->get_icon(0) == get_theme_icon("Warning", "EditorIcons")) {
		type = "W ";
	} else if (ti->get_icon(0) == get_theme_icon("Error", "EditorIcons")) {
		type = "E ";
	}

	String text = ti->get_text(0) + "   ";
	int rpad_len = text.length();

	text = type + text + ti->get_text(1) + "\n";
	TreeItem *ci = ti->get_children();
	while (ci) {
		text += "  " + ci->get_text(0).rpad(rpad_len) + ci->get_text(1) + "\n";
		ci = ci->get_next();
	}

	DisplayServer::get_singleton()->clipboard_set(text);
}

void ScriptEditorDebugger::_tab_changed(int p_tab) {
	if (tabs->get_tab_title(p_tab) == TTR("Video RAM")) {
		// "Video RAM" tab was clicked, refresh the data it's displaying when entering the tab.
		_video_mem_request();
	}
}

void ScriptEditorDebugger::_bind_methods() {

	ClassDB::bind_method(D_METHOD("live_debug_create_node"), &ScriptEditorDebugger::live_debug_create_node);
	ClassDB::bind_method(D_METHOD("live_debug_instance_node"), &ScriptEditorDebugger::live_debug_instance_node);
	ClassDB::bind_method(D_METHOD("live_debug_remove_node"), &ScriptEditorDebugger::live_debug_remove_node);
	ClassDB::bind_method(D_METHOD("live_debug_remove_and_keep_node"), &ScriptEditorDebugger::live_debug_remove_and_keep_node);
	ClassDB::bind_method(D_METHOD("live_debug_restore_node"), &ScriptEditorDebugger::live_debug_restore_node);
	ClassDB::bind_method(D_METHOD("live_debug_duplicate_node"), &ScriptEditorDebugger::live_debug_duplicate_node);
	ClassDB::bind_method(D_METHOD("live_debug_reparent_node"), &ScriptEditorDebugger::live_debug_reparent_node);
	ClassDB::bind_method(D_METHOD("request_remote_object", "id"), &ScriptEditorDebugger::request_remote_object);
	ClassDB::bind_method(D_METHOD("update_remote_object", "id", "property", "value"), &ScriptEditorDebugger::update_remote_object);

	ADD_SIGNAL(MethodInfo("stopped"));
	ADD_SIGNAL(MethodInfo("stop_requested"));
	ADD_SIGNAL(MethodInfo("stack_frame_selected", PropertyInfo(Variant::INT, "frame")));
	ADD_SIGNAL(MethodInfo("error_selected", PropertyInfo(Variant::INT, "error")));
	ADD_SIGNAL(MethodInfo("set_execution", PropertyInfo("script"), PropertyInfo(Variant::INT, "line")));
	ADD_SIGNAL(MethodInfo("clear_execution", PropertyInfo("script")));
	ADD_SIGNAL(MethodInfo("breaked", PropertyInfo(Variant::BOOL, "reallydid"), PropertyInfo(Variant::BOOL, "can_debug")));
	ADD_SIGNAL(MethodInfo("remote_object_requested", PropertyInfo(Variant::INT, "id")));
	ADD_SIGNAL(MethodInfo("remote_object_updated", PropertyInfo(Variant::INT, "id")));
	ADD_SIGNAL(MethodInfo("remote_object_property_updated", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::STRING, "property")));
	ADD_SIGNAL(MethodInfo("remote_tree_updated"));
}

ScriptEditorDebugger::ScriptEditorDebugger(EditorNode *p_editor) {

	editor = p_editor;

	tabs = memnew(TabContainer);
	tabs->set_tab_align(TabContainer::ALIGN_LEFT);
	tabs->add_theme_style_override("panel", editor->get_gui_base()->get_theme_stylebox("DebuggerPanel", "EditorStyles"));
	tabs->connect("tab_changed", callable_mp(this, &ScriptEditorDebugger::_tab_changed));

	add_child(tabs);

	{ //debugger
		VBoxContainer *vbc = memnew(VBoxContainer);
		vbc->set_name(TTR("Debugger"));
		Control *dbg = vbc;

		HBoxContainer *hbc = memnew(HBoxContainer);
		vbc->add_child(hbc);

		reason = memnew(Label);
		reason->set_text("");
		hbc->add_child(reason);
		reason->set_h_size_flags(SIZE_EXPAND_FILL);
		reason->set_autowrap(true);
		reason->set_max_lines_visible(3);
		reason->set_mouse_filter(Control::MOUSE_FILTER_PASS);

		hbc->add_child(memnew(VSeparator));

		skip_breakpoints = memnew(ToolButton);
		hbc->add_child(skip_breakpoints);
		skip_breakpoints->set_tooltip(TTR("Skip Breakpoints"));
		skip_breakpoints->connect("pressed", callable_mp(this, &ScriptEditorDebugger::debug_skip_breakpoints));

		hbc->add_child(memnew(VSeparator));

		copy = memnew(ToolButton);
		hbc->add_child(copy);
		copy->set_tooltip(TTR("Copy Error"));
		copy->connect("pressed", callable_mp(this, &ScriptEditorDebugger::debug_copy));

		hbc->add_child(memnew(VSeparator));

		step = memnew(ToolButton);
		hbc->add_child(step);
		step->set_tooltip(TTR("Step Into"));
		step->set_shortcut(ED_GET_SHORTCUT("debugger/step_into"));
		step->connect("pressed", callable_mp(this, &ScriptEditorDebugger::debug_step));

		next = memnew(ToolButton);
		hbc->add_child(next);
		next->set_tooltip(TTR("Step Over"));
		next->set_shortcut(ED_GET_SHORTCUT("debugger/step_over"));
		next->connect("pressed", callable_mp(this, &ScriptEditorDebugger::debug_next));

		hbc->add_child(memnew(VSeparator));

		dobreak = memnew(ToolButton);
		hbc->add_child(dobreak);
		dobreak->set_tooltip(TTR("Break"));
		dobreak->set_shortcut(ED_GET_SHORTCUT("debugger/break"));
		dobreak->connect("pressed", callable_mp(this, &ScriptEditorDebugger::debug_break));

		docontinue = memnew(ToolButton);
		hbc->add_child(docontinue);
		docontinue->set_tooltip(TTR("Continue"));
		docontinue->set_shortcut(ED_GET_SHORTCUT("debugger/continue"));
		docontinue->connect("pressed", callable_mp(this, &ScriptEditorDebugger::debug_continue));

		HSplitContainer *sc = memnew(HSplitContainer);
		vbc->add_child(sc);
		sc->set_v_size_flags(SIZE_EXPAND_FILL);

		stack_dump = memnew(Tree);
		stack_dump->set_allow_reselect(true);
		stack_dump->set_columns(1);
		stack_dump->set_column_titles_visible(true);
		stack_dump->set_column_title(0, TTR("Stack Frames"));
		stack_dump->set_h_size_flags(SIZE_EXPAND_FILL);
		stack_dump->set_hide_root(true);
		stack_dump->connect("cell_selected", callable_mp(this, &ScriptEditorDebugger::_stack_dump_frame_selected));
		sc->add_child(stack_dump);

		inspector = memnew(EditorDebuggerInspector);
		inspector->set_h_size_flags(SIZE_EXPAND_FILL);
		inspector->set_enable_capitalize_paths(false);
		inspector->set_read_only(true);
		inspector->connect("object_selected", callable_mp(this, &ScriptEditorDebugger::_remote_object_selected));
		inspector->connect("object_edited", callable_mp(this, &ScriptEditorDebugger::_remote_object_edited));
		inspector->connect("object_property_updated", callable_mp(this, &ScriptEditorDebugger::_remote_object_property_updated));
		sc->add_child(inspector);
		tabs->add_child(dbg);
	}

	{ //errors
		errors_tab = memnew(VBoxContainer);
		errors_tab->set_name(TTR("Errors"));

		HBoxContainer *errhb = memnew(HBoxContainer);
		errors_tab->add_child(errhb);

		Button *expand_all = memnew(Button);
		expand_all->set_text(TTR("Expand All"));
		expand_all->connect("pressed", callable_mp(this, &ScriptEditorDebugger::_expand_errors_list));
		errhb->add_child(expand_all);

		Button *collapse_all = memnew(Button);
		collapse_all->set_text(TTR("Collapse All"));
		collapse_all->connect("pressed", callable_mp(this, &ScriptEditorDebugger::_collapse_errors_list));
		errhb->add_child(collapse_all);

		Control *space = memnew(Control);
		space->set_h_size_flags(SIZE_EXPAND_FILL);
		errhb->add_child(space);

		clearbutton = memnew(Button);
		clearbutton->set_text(TTR("Clear"));
		clearbutton->set_h_size_flags(0);
		clearbutton->connect("pressed", callable_mp(this, &ScriptEditorDebugger::_clear_errors_list));
		errhb->add_child(clearbutton);

		error_tree = memnew(Tree);
		error_tree->set_columns(2);

		error_tree->set_column_expand(0, false);
		error_tree->set_column_min_width(0, 140);

		error_tree->set_column_expand(1, true);

		error_tree->set_select_mode(Tree::SELECT_ROW);
		error_tree->set_hide_root(true);
		error_tree->set_v_size_flags(SIZE_EXPAND_FILL);
		error_tree->set_allow_rmb_select(true);
		error_tree->connect("item_rmb_selected", callable_mp(this, &ScriptEditorDebugger::_error_tree_item_rmb_selected));
		errors_tab->add_child(error_tree);

		item_menu = memnew(PopupMenu);
		item_menu->connect("id_pressed", callable_mp(this, &ScriptEditorDebugger::_item_menu_id_pressed));
		error_tree->add_child(item_menu);

		tabs->add_child(errors_tab);
	}

	{ // File dialog
		file_dialog = memnew(EditorFileDialog);
		file_dialog->connect("file_selected", callable_mp(this, &ScriptEditorDebugger::_file_selected));
		add_child(file_dialog);
	}

	{ //profiler
		profiler = memnew(EditorProfiler);
		profiler->set_name(TTR("Profiler"));
		tabs->add_child(profiler);
		profiler->connect("enable_profiling", callable_mp(this, &ScriptEditorDebugger::_profiler_activate), varray(PROFILER_SCRIPTS_SERVERS));
		profiler->connect("break_request", callable_mp(this, &ScriptEditorDebugger::_profiler_seeked));
	}

	{ //frame profiler
		visual_profiler = memnew(EditorVisualProfiler);
		visual_profiler->set_name(TTR("Visual Profiler"));
		tabs->add_child(visual_profiler);
		visual_profiler->connect("enable_profiling", callable_mp(this, &ScriptEditorDebugger::_profiler_activate), varray(PROFILER_VISUAL));
	}

	{ //network profiler
		network_profiler = memnew(EditorNetworkProfiler);
		network_profiler->set_name(TTR("Network Profiler"));
		tabs->add_child(network_profiler);
		network_profiler->connect("enable_profiling", callable_mp(this, &ScriptEditorDebugger::_profiler_activate), varray(PROFILER_NETWORK));
	}

	{ //monitors

		HSplitContainer *hsp = memnew(HSplitContainer);

		perf_monitors = memnew(Tree);
		perf_monitors->set_columns(2);
		perf_monitors->set_column_title(0, TTR("Monitor"));
		perf_monitors->set_column_title(1, TTR("Value"));
		perf_monitors->set_column_titles_visible(true);
		perf_monitors->connect("item_edited", callable_mp(this, &ScriptEditorDebugger::_performance_select));
		hsp->add_child(perf_monitors);

		perf_draw = memnew(Control);
		perf_draw->set_clip_contents(true);
		perf_draw->connect("draw", callable_mp(this, &ScriptEditorDebugger::_performance_draw));
		hsp->add_child(perf_draw);

		hsp->set_name(TTR("Monitors"));
		hsp->set_split_offset(340 * EDSCALE);
		tabs->add_child(hsp);
		perf_max.resize(Performance::MONITOR_MAX);

		Map<String, TreeItem *> bases;
		TreeItem *root = perf_monitors->create_item();
		perf_monitors->set_hide_root(true);
		for (int i = 0; i < Performance::MONITOR_MAX; i++) {

			String n = Performance::get_singleton()->get_monitor_name(Performance::Monitor(i));
			Performance::MonitorType mtype = Performance::get_singleton()->get_monitor_type(Performance::Monitor(i));
			String base = n.get_slice("/", 0);
			String name = n.get_slice("/", 1);
			if (!bases.has(base)) {
				TreeItem *b = perf_monitors->create_item(root);
				b->set_text(0, base.capitalize());
				b->set_editable(0, false);
				b->set_selectable(0, false);
				b->set_expand_right(0, true);
				bases[base] = b;
			}

			TreeItem *it = perf_monitors->create_item(bases[base]);
			it->set_metadata(1, mtype);
			it->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
			it->set_editable(0, true);
			it->set_selectable(0, false);
			it->set_selectable(1, false);
			it->set_text(0, name.capitalize());
			perf_items.push_back(it);
			perf_max.write[i] = 0;
		}

		info_message = memnew(Label);
		info_message->set_text(TTR("Pick one or more items from the list to display the graph."));
		info_message->set_valign(Label::VALIGN_CENTER);
		info_message->set_align(Label::ALIGN_CENTER);
		info_message->set_autowrap(true);
		info_message->set_custom_minimum_size(Size2(100 * EDSCALE, 0));
		info_message->set_anchors_and_margins_preset(PRESET_WIDE, PRESET_MODE_KEEP_SIZE, 8 * EDSCALE);
		perf_draw->add_child(info_message);
	}

	{ //vmem inspect
		VBoxContainer *vmem_vb = memnew(VBoxContainer);
		HBoxContainer *vmem_hb = memnew(HBoxContainer);
		Label *vmlb = memnew(Label(TTR("List of Video Memory Usage by Resource:") + " "));
		vmlb->set_h_size_flags(SIZE_EXPAND_FILL);
		vmem_hb->add_child(vmlb);
		vmem_hb->add_child(memnew(Label(TTR("Total:") + " ")));
		vmem_total = memnew(LineEdit);
		vmem_total->set_editable(false);
		vmem_total->set_custom_minimum_size(Size2(100, 0) * EDSCALE);
		vmem_hb->add_child(vmem_total);
		vmem_refresh = memnew(ToolButton);
		vmem_hb->add_child(vmem_refresh);
		vmem_vb->add_child(vmem_hb);
		vmem_refresh->connect("pressed", callable_mp(this, &ScriptEditorDebugger::_video_mem_request));

		VBoxContainer *vmmc = memnew(VBoxContainer);
		vmem_tree = memnew(Tree);
		vmem_tree->set_v_size_flags(SIZE_EXPAND_FILL);
		vmem_tree->set_h_size_flags(SIZE_EXPAND_FILL);
		vmmc->add_child(vmem_tree);
		vmmc->set_v_size_flags(SIZE_EXPAND_FILL);
		vmem_vb->add_child(vmmc);

		vmem_vb->set_name(TTR("Video RAM"));
		vmem_tree->set_columns(4);
		vmem_tree->set_column_titles_visible(true);
		vmem_tree->set_column_title(0, TTR("Resource Path"));
		vmem_tree->set_column_expand(0, true);
		vmem_tree->set_column_expand(1, false);
		vmem_tree->set_column_title(1, TTR("Type"));
		vmem_tree->set_column_min_width(1, 100 * EDSCALE);
		vmem_tree->set_column_expand(2, false);
		vmem_tree->set_column_title(2, TTR("Format"));
		vmem_tree->set_column_min_width(2, 150 * EDSCALE);
		vmem_tree->set_column_expand(3, false);
		vmem_tree->set_column_title(3, TTR("Usage"));
		vmem_tree->set_column_min_width(3, 80 * EDSCALE);
		vmem_tree->set_hide_root(true);

		tabs->add_child(vmem_vb);
	}

	{ // misc
		VBoxContainer *misc = memnew(VBoxContainer);
		misc->set_name(TTR("Misc"));
		tabs->add_child(misc);

		GridContainer *info_left = memnew(GridContainer);
		info_left->set_columns(2);
		misc->add_child(info_left);
		clicked_ctrl = memnew(LineEdit);
		clicked_ctrl->set_h_size_flags(SIZE_EXPAND_FILL);
		info_left->add_child(memnew(Label(TTR("Clicked Control:"))));
		info_left->add_child(clicked_ctrl);
		clicked_ctrl_type = memnew(LineEdit);
		info_left->add_child(memnew(Label(TTR("Clicked Control Type:"))));
		info_left->add_child(clicked_ctrl_type);

		scene_tree = memnew(SceneDebuggerTree);
		live_edit_root = memnew(LineEdit);
		live_edit_root->set_h_size_flags(SIZE_EXPAND_FILL);

		{
			HBoxContainer *lehb = memnew(HBoxContainer);
			Label *l = memnew(Label(TTR("Live Edit Root:")));
			info_left->add_child(l);
			lehb->add_child(live_edit_root);
			le_set = memnew(Button(TTR("Set From Tree")));
			lehb->add_child(le_set);
			le_clear = memnew(Button(TTR("Clear")));
			lehb->add_child(le_clear);
			info_left->add_child(lehb);
		}

		misc->add_child(memnew(VSeparator));

		HBoxContainer *buttons = memnew(HBoxContainer);

		export_csv = memnew(Button(TTR("Export measures as CSV")));
		export_csv->connect("pressed", callable_mp(this, &ScriptEditorDebugger::_export_csv));
		buttons->add_child(export_csv);

		misc->add_child(buttons);
	}

	msgdialog = memnew(AcceptDialog);
	add_child(msgdialog);

	live_debug = true;
	camera_override = CameraOverride::OVERRIDE_NONE;
	last_path_id = false;
	error_count = 0;
	warning_count = 0;
	_update_buttons_state();
}

ScriptEditorDebugger::~ScriptEditorDebugger() {

	if (peer.is_valid()) {
		peer->close();
		peer.unref();
	}
	memdelete(scene_tree);
}
