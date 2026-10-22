#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert an LVGL XML UI project into C for LVGL 9.

Covers the subset of the LVGL XML format used by helpers/ui-lvgl/xml: globals
(consts, styles, subjects, fonts), components with an <api>, screens, the
built-in widgets listed in WIDGETS, local style properties, style/bind/event
child elements. Anything else is an error, so a construct this script does not
understand never turns into silently missing UI.

Every generated C symbol gets the --prefix (default "mcui_"); event_cb
callbacks keep the name written in the XML and are left to the application.

Usage:
    lvgl_xml2c.py <xml project dir> <output dir> [--prefix mcui_]
                  [--font-conv /path/to/lv_font_conv] [--font-fallback lv_font_montserrat_14]
                  [--check]

--check regenerates into a temporary directory and fails if the output
differs from <output dir> (for CI). Fonts are only converted when
--font-conv is given; otherwise the existing font C files are kept.
--font-fallback names a built-in LVGL font every converted font falls back
to, so glyphs outside the converted range (LV_SYMBOL_* used by keyboards and
dropdowns) still draw.
"""

import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

STRING_SUBJECT_SIZE = 64


class GenError(Exception):
	pass


def c_str(s):
	out = s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t')
	return '"' + out + '"'


def ident(name):
	if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
		raise GenError(f'not a valid C identifier: {name!r}')
	return name


# ---------------------------------------------------------------- enums
ALIGN = ['default', 'top_left', 'top_mid', 'top_right', 'bottom_left', 'bottom_mid', 'bottom_right',
	 'left_mid', 'right_mid', 'center']
FLEX_FLOW = ['row', 'column', 'row_wrap', 'row_reverse', 'row_wrap_reverse', 'column_wrap',
	     'column_reverse', 'column_wrap_reverse']
FLEX_ALIGN = ['start', 'end', 'center', 'space_evenly', 'space_around', 'space_between']
TEXT_ALIGN = ['auto', 'left', 'center', 'right']
BORDER_SIDE = ['none', 'bottom', 'top', 'left', 'right', 'full', 'internal']
DIR = ['none', 'left', 'right', 'top', 'bottom', 'hor', 'ver', 'all']
SCROLLBAR = ['off', 'on', 'active', 'auto']
LONG_MODE = ['wrap', 'dots', 'scroll', 'scroll_circular', 'clip']
KB_MODE = ['text_lower', 'text_upper', 'special', 'number', 'user_1', 'user_2', 'user_3', 'user_4']
LAYOUT = ['none', 'flex', 'grid']
SCREEN_ANIM = ['none', 'over_left', 'over_right', 'over_top', 'over_bottom', 'move_left', 'move_right',
	       'move_top', 'move_bottom', 'fade_in', 'fade_on', 'fade_out', 'out_left', 'out_right',
	       'out_top', 'out_bottom']
PARTS = ['main', 'scrollbar', 'indicator', 'knob', 'selected', 'items', 'cursor']
STATES = ['default', 'checked', 'focused', 'focus_key', 'edited', 'hovered', 'pressed', 'scrolled',
	  'disabled', 'user_1', 'user_2', 'user_3', 'user_4']
FLAGS = {'hidden': 'HIDDEN', 'clickable': 'CLICKABLE', 'scrollable': 'SCROLLABLE',
	 'checkable': 'CHECKABLE', 'click_focusable': 'CLICK_FOCUSABLE', 'floating': 'FLOATING',
	 'ignore_layout': 'IGNORE_LAYOUT', 'event_bubble': 'EVENT_BUBBLE'}


def enum(value, options, prefix, what):
	if value not in options:
		raise GenError(f'{what}: unknown value {value!r} (expected one of {options})')
	return prefix + value.upper()


def selector(text, sep):
	if not text:
		return '0'
	parts = []
	for item in text.split(sep):
		if item in PARTS:
			parts.append('LV_PART_' + item.upper())
		elif item in STATES:
			parts.append('LV_STATE_' + item.upper())
		else:
			raise GenError(f'unknown style selector {item!r}')
	return ' | '.join(parts)


def event_code(trigger):
	if not re.fullmatch(r'[a-z_]+', trigger or ''):
		raise GenError(f'bad trigger {trigger!r}')
	return 'LV_EVENT_' + trigger.upper()


# ---------------------------------------------------------------- project model
class Prop:
	def __init__(self, name, ptype, default):
		self.name, self.type, self.default = name, ptype, default


class Component:
	def __init__(self, name, root, is_screen):
		self.name, self.root, self.is_screen = name, root, is_screen
		self.props = []
		self.consts = {}
		self.styles = []
		self.permanent = root.get('permanent') == 'true'
		for child in root:
			if child.tag == 'api':
				for p in child:
					if p.tag != 'prop':
						raise GenError(f'{name}: <api> may only contain <prop>, got <{p.tag}>')
					self.props.append(Prop(p.get('name'), p.get('type'), p.get('default')))
			elif child.tag == 'consts':
				for c in child:
					self.consts[c.get('name')] = (c.tag, c.get('value'))
			elif child.tag == 'styles':
				self.styles.extend(child)
			elif child.tag in ('view', 'previews'):
				pass
			else:
				raise GenError(f'{name}: unsupported top-level <{child.tag}>')
		self.view = root.find('view')
		if self.view is None:
			raise GenError(f'{name}: no <view>')
		self.extends = self.view.get('extends', 'lv_obj')
		if is_screen and 'extends' in self.view.attrib:
			raise GenError(f'{name}: screens cannot extend')


class Project:
	def __init__(self, root_dir, prefix):
		self.dir = root_dir
		self.p = prefix
		g = ET.parse(os.path.join(root_dir, 'globals.xml')).getroot()
		self.consts = {}
		self.styles = []
		self.subjects = []
		self.fonts = []
		for sec in g:
			if sec.tag == 'consts':
				for c in sec:
					self.consts[c.get('name')] = (c.tag, c.get('value'))
			elif sec.tag == 'styles':
				self.styles.extend(sec)
			elif sec.tag == 'subjects':
				self.subjects.extend(sec)
			elif sec.tag == 'fonts':
				self.fonts.extend(sec)
			elif sec.tag in ('api', 'images'):
				if len(sec):
					raise GenError(f'globals: <{sec.tag}> entries are not supported')
			else:
				raise GenError(f'globals: unsupported <{sec.tag}>')
		self.style_names = {s.get('name') for s in self.styles}
		self.subject_types = {s.get('name'): s.tag for s in self.subjects}
		self.font_names = {f.get('name') for f in self.fonts}
		self.components = {}
		for sub, is_screen in (('components', False), ('screens', True)):
			d = os.path.join(root_dir, sub)
			if not os.path.isdir(d):
				continue
			for fn in sorted(os.listdir(d)):
				if fn.endswith('.xml'):
					name = fn[:-4]
					root = ET.parse(os.path.join(d, fn)).getroot()
					want = 'screen' if is_screen else 'component'
					if root.tag != want:
						raise GenError(f'{sub}/{fn}: root must be <{want}>')
					self.components[ident(name)] = Component(name, root, is_screen)

	def base_widget(self, comp_name):
		seen = set()
		name = comp_name
		while name in self.components:
			if name in seen:
				raise GenError(f'extends loop at {name}')
			seen.add(name)
			name = self.components[name].extends
		return name


# ---------------------------------------------------------------- code generation
class Ctx:
	"""Per-component generation state."""

	def __init__(self, proj, comp):
		self.proj = proj
		self.comp = comp
		self.lines = []
		self.counter = 0
		self.externs = set()
		self.helpers = {}
		self.params = {p.name: p for p in comp.props} if comp else {}

	def var(self):
		self.counter += 1
		return f'obj{self.counter}'

	def emit(self, depth, line):
		self.lines.append('\t' * depth + line)

	# ---- value resolution
	def const(self, name):
		if self.comp and name in self.comp.consts:
			return self.comp.consts[name]
		if name in self.proj.consts:
			return self.proj.consts[name]
		raise GenError(f'unknown constant #{name}')

	def resolve(self, raw, kind, what):
		if raw is None:
			raise GenError(f'{what}: missing value')
		if raw.startswith('$'):
			pname = raw[1:]
			if pname not in self.params:
				raise GenError(f'{what}: unknown property ${pname}')
			return pname
		if raw.startswith('#') and kind != 'color_literal':
			ctype, cval = self.const(raw[1:])
			return self.resolve(cval, kind, what)
		return self.literal(raw, kind, what)

	def literal(self, raw, kind, what):
		p = self.proj.p
		if kind == 'string':
			return c_str(raw)
		if kind == 'int':
			if not re.fullmatch(r'-?\d+', raw.replace('px', '')):
				raise GenError(f'{what}: expected an integer, got {raw!r}')
			return raw.replace('px', '')
		if kind == 'size':
			if raw == 'content':
				return 'LV_SIZE_CONTENT'
			if raw.endswith('%'):
				return f'lv_pct({int(raw[:-1])})'
			return self.literal(raw, 'int', what)
		if kind == 'opa':
			if raw.endswith('%'):
				return f'(lv_opa_t)({int(raw[:-1])} * 255 / 100)'
			return self.literal(raw, 'int', what)
		if kind == 'bool':
			if raw not in ('true', 'false'):
				raise GenError(f'{what}: expected true/false, got {raw!r}')
			return raw
		if kind in ('color', 'color_literal'):
			v = raw.lstrip('#')
			if v.startswith('0x'):
				v = v[2:]
			if len(v) == 3:
				v = ''.join(ch * 2 for ch in v)
			if not re.fullmatch(r'[0-9a-fA-F]{6}', v):
				raise GenError(f'{what}: bad color {raw!r}')
			return f'lv_color_hex(0x{v.lower()})'
		if kind == 'font':
			if raw not in self.proj.font_names:
				raise GenError(f'{what}: unknown font {raw!r}')
			return f'&{p}{raw}'
		if kind == 'subject':
			if raw not in self.proj.subject_types:
				raise GenError(f'{what}: unknown subject {raw!r}')
			return f'{p}{raw}'
		if kind == 'style':
			if raw not in self.proj.style_names:
				raise GenError(f'{what}: unknown style {raw!r}')
			return f'&{p}{raw}'
		raise GenError(f'{what}: unhandled kind {kind}')

	def subject_ref(self, raw, what):
		"""A subject reference, either by name or through a subject-typed property."""
		v = self.resolve(raw, 'subject', what)
		return v


STYLE_ENUMS = {
	'border_side': (BORDER_SIDE, 'LV_BORDER_SIDE_'),
	'flex_main_place': (FLEX_ALIGN, 'LV_FLEX_ALIGN_'),
	'flex_cross_place': (FLEX_ALIGN, 'LV_FLEX_ALIGN_'),
	'flex_track_place': (FLEX_ALIGN, 'LV_FLEX_ALIGN_'),
	'flex_flow': (FLEX_FLOW, 'LV_FLEX_FLOW_'),
	'text_align': (TEXT_ALIGN, 'LV_TEXT_ALIGN_'),
	'layout': (LAYOUT, 'LV_LAYOUT_'),
}
STYLE_SIZE = {'width', 'height', 'min_width', 'max_width', 'min_height', 'max_height'}


def style_value(ctx, prop, raw, what):
	if not re.fullmatch(r'[a-z_]+', prop):
		raise GenError(f'{what}: bad style property {prop!r}')
	if prop in STYLE_ENUMS:
		if raw.startswith('$'):
			return ctx.resolve(raw, 'int', what)
		opts, pre = STYLE_ENUMS[prop]
		return enum(raw, opts, pre, what)
	if prop.endswith('_color'):
		return ctx.resolve(raw, 'color', what)
	if prop == 'text_font':
		return ctx.resolve(raw, 'font', what)
	if prop == 'opa' or prop.endswith('_opa'):
		return ctx.resolve(raw, 'opa', what)
	if prop in STYLE_SIZE:
		return ctx.resolve(raw, 'size', what)
	return ctx.resolve(raw, 'int', what)


# Widget creation and attribute setters.
WIDGETS = {
	'lv_obj': 'lv_obj_create', 'lv_label': 'lv_label_create', 'lv_button': 'lv_button_create',
	'lv_bar': 'lv_bar_create', 'lv_slider': 'lv_slider_create', 'lv_switch': 'lv_switch_create',
	'lv_dropdown': 'lv_dropdown_create', 'lv_textarea': 'lv_textarea_create',
	'lv_keyboard': 'lv_keyboard_create', 'lv_tabview': 'lv_tabview_create',
}


def apply_attr(ctx, depth, v, widget, name, raw, attrs):
	what = f'{ctx.comp.name if ctx.comp else "?"}: <{widget}> {name}'
	e = lambda line: ctx.emit(depth, line)
	if name.startswith('style_'):
		prop, _, sel = name[6:].partition('-')
		e(f'lv_obj_set_style_{prop}({v}, {style_value(ctx, prop, raw, what)}, {selector(sel, "-")});')
		return
	if name in ('width', 'height'):
		e(f'lv_obj_set_{name}({v}, {ctx.resolve(raw, "size", what)});')
	elif name in ('x', 'y'):
		e(f'lv_obj_set_{name}({v}, {ctx.resolve(raw, "size", what)});')
	elif name == 'align':
		e(f'lv_obj_set_align({v}, {enum(raw, ALIGN, "LV_ALIGN_", what)});')
	elif name == 'flex_flow':
		e(f'lv_obj_set_flex_flow({v}, {enum(raw, FLEX_FLOW, "LV_FLEX_FLOW_", what)});')
	elif name == 'flex_grow':
		e(f'lv_obj_set_flex_grow({v}, {ctx.resolve(raw, "int", what)});')
	elif name in FLAGS:
		e(f'lv_obj_set_{name}({v}, {ctx.resolve(raw, "bool", what)});')
	elif name == 'scrollbar_mode':
		e(f'lv_obj_set_scrollbar_mode({v}, {enum(raw, SCROLLBAR, "LV_SCROLLBAR_MODE_", what)});')
	elif name == 'name':
		e(f'lv_obj_set_name({v}, {c_str(raw)});')
	elif widget == 'lv_label' and name == 'text':
		e(f'lv_label_set_text({v}, {ctx.resolve(raw, "string", what)});')
	elif widget == 'lv_label' and name == 'long_mode':
		e(f'lv_label_set_long_mode({v}, {enum(raw, LONG_MODE, "LV_LABEL_LONG_MODE_", what)});')
	elif widget == 'lv_label' and name == 'bind_text':
		fmt = attrs.get('bind_text-fmt')
		e(f'lv_label_bind_text({v}, {ctx.subject_ref(raw, what)}, {c_str(fmt) if fmt else "NULL"});')
	elif widget == 'lv_label' and name == 'bind_text-fmt':
		pass
	elif widget in ('lv_bar', 'lv_slider') and name == 'bind_value':
		e(f'{widget}_bind_value({v}, {ctx.subject_ref(raw, what)});')
	elif widget in ('lv_bar', 'lv_slider') and name in ('min_value', 'max_value'):
		lo = ctx.resolve(attrs.get('min_value', '0'), 'int', what)
		hi = ctx.resolve(attrs.get('max_value', '100'), 'int', what)
		if name == 'min_value' or 'min_value' not in attrs:
			e(f'{widget}_set_range({v}, {lo}, {hi});')
	elif widget == 'lv_switch' and name == 'bind_checked':
		e(f'lv_obj_bind_checked({v}, {ctx.subject_ref(raw, what)});')
	elif widget == 'lv_dropdown' and name == 'options':
		e(f'lv_dropdown_set_options({v}, {ctx.resolve(raw, "string", what)});')
	elif widget == 'lv_dropdown' and name == 'bind_value':
		e(f'lv_dropdown_bind_value({v}, {ctx.subject_ref(raw, what)});')
	elif widget == 'lv_textarea' and name == 'one_line':
		e(f'lv_textarea_set_one_line({v}, {ctx.resolve(raw, "bool", what)});')
	elif widget == 'lv_textarea' and name == 'placeholder_text':
		e(f'lv_textarea_set_placeholder_text({v}, {ctx.resolve(raw, "string", what)});')
	elif widget == 'lv_keyboard' and name == 'mode':
		e(f'lv_keyboard_set_mode({v}, {enum(raw, KB_MODE, "LV_KEYBOARD_MODE_", what)});')
	elif widget == 'lv_tabview' and name == 'tab_bar_position':
		e(f'lv_tabview_set_tab_bar_position({v}, {enum(raw, DIR, "LV_DIR_", what)});')
	elif widget == 'lv_tabview' and name == 'active':
		e(f'lv_tabview_set_active({v}, {ctx.resolve(raw, "int", what)}, LV_ANIM_OFF);')
	else:
		raise GenError(f'{what}: unsupported attribute')


def apply_attrs(ctx, depth, v, widget, attrs, skip=()):
	for name, raw in attrs.items():
		if name in skip:
			continue
		apply_attr(ctx, depth, v, widget, name, raw, attrs)


SPECIAL_CHILDREN = {'style', 'bind_style', 'event_cb', 'subject_set_string_event', 'subject_set_int_event',
		    'subject_toggle_event', 'subject_increment_event', 'screen_load_event', 'screen_create_event'}
BIND_RE = re.compile(r'(?:lv_obj-)?bind_(flag|state)_if_(eq|not_eq|gt|ge|lt|le)$')


def gen_special(ctx, depth, v, el):
	p = ctx.proj.p
	tag = el.tag[7:] if el.tag.startswith('lv_obj-') else el.tag
	a = el.attrib
	what = f'{ctx.comp.name}: <{el.tag}>'
	e = lambda line: ctx.emit(depth, line)
	m = BIND_RE.match(tag)
	if m:
		kind, op = m.groups()
		target = a.get(kind)
		if kind == 'flag':
			if target not in FLAGS:
				raise GenError(f'{what}: unknown flag {target!r}')
			setter = f'lv_obj_set_{target}(obj, {{cond}})'
		else:
			setter = f'lv_obj_set_state(obj, {enum(target, STATES, "LV_STATE_", what)}, {{cond}})'
		ref = ctx.resolve(a['ref_value'], 'int', what)
		if not re.fullmatch(r'-?\d+', ref):
			raise GenError(f'{what}: ref_value must be a literal integer')
		cmp_op = {'eq': '==', 'not_eq': '!=', 'gt': '>', 'ge': '>=', 'lt': '<', 'le': '<='}[op]
		fn = f'bind_{target}_if_{op}_{ref.replace("-", "m")}'
		ctx.helpers[fn] = (f'static void {fn}(lv_observer_t * observer, lv_subject_t * subject)\n{{\n'
				   f'\tlv_obj_t * obj = lv_observer_get_target_obj(observer);\n\n'
				   f'\t{setter.format(cond=f"lv_subject_get_int(subject) {cmp_op} {ref}")};\n}}\n')
		e(f'lv_subject_add_observer_obj({ctx.subject_ref(a["subject"], what)}, {fn}, {v}, NULL);')
	elif tag == 'style':
		e(f'lv_obj_add_style({v}, {ctx.resolve(a["name"], "style", what)}, {selector(a.get("selector"), "|")});')
	elif tag == 'bind_style':
		e(f'lv_obj_bind_style({v}, {ctx.resolve(a["name"], "style", what)}, {selector(a.get("selector"), "|")}, '
		  f'{ctx.subject_ref(a["subject"], what)}, {ctx.resolve(a["ref_value"], "int", what)});')
	elif tag == 'event_cb':
		cb = ident(a['callback'])
		ctx.externs.add(f'void {cb}(lv_event_t * e);')
		ud = a.get('user_data')
		e(f'lv_obj_add_event_cb({v}, {cb}, {event_code(a.get("trigger", "clicked"))}, '
		  f'{"(void *)" + c_str(ud) if ud else "NULL"});')
	elif tag == 'subject_set_string_event':
		e(f'lv_obj_add_subject_set_string_event({v}, {ctx.subject_ref(a["subject"], what)}, '
		  f'{event_code(a.get("trigger", "clicked"))}, {ctx.resolve(a["value"], "string", what)});')
	elif tag == 'subject_set_int_event':
		e(f'lv_obj_add_subject_set_int_event({v}, {ctx.subject_ref(a["subject"], what)}, '
		  f'{event_code(a.get("trigger", "clicked"))}, {ctx.resolve(a["value"], "int", what)});')
	elif tag == 'subject_toggle_event':
		e(f'lv_obj_add_subject_toggle_event({v}, {ctx.subject_ref(a["subject"], what)}, '
		  f'{event_code(a.get("trigger", "clicked"))});')
	elif tag == 'subject_increment_event':
		e(f'lv_obj_add_subject_increment_event({v}, {ctx.subject_ref(a["subject"], what)}, '
		  f'{event_code(a.get("trigger", "clicked"))}, {ctx.resolve(a.get("step", "1"), "int", what)});')
	elif tag in ('screen_load_event', 'screen_create_event'):
		scr = a['screen']
		target = ctx.proj.components.get(scr)
		if target is None or not target.is_screen:
			raise GenError(f'{what}: unknown screen {scr!r}')
		anim = enum(a.get('anim_type', 'none'), SCREEN_ANIM, 'LV_SCREEN_LOAD_ANIM_', what)
		dur = ctx.resolve(a.get('duration', '0'), 'int', what)
		delay = ctx.resolve(a.get('delay', '0'), 'int', what)
		trig = event_code(a.get('trigger', 'clicked'))
		if tag == 'screen_load_event':
			if not target.permanent:
				raise GenError(f'{what}: screen_load_event needs a permanent screen ({scr})')
			e(f'lv_obj_add_screen_load_event({v}, {trig}, {p}{scr}, {anim}, {dur}, {delay});')
		else:
			e(f'lv_obj_add_screen_create_event({v}, {trig}, {p}{scr}_create, {anim}, {dur}, {delay});')
	else:
		raise GenError(f'{what}: unsupported element')


def gen_children(ctx, depth, v, el):
	for child in el:
		if child.tag in SPECIAL_CHILDREN or BIND_RE.match(child.tag[7:] if child.tag.startswith('lv_obj-') else child.tag):
			gen_special(ctx, depth, v, child)
		else:
			gen_element(ctx, depth, v, child)


def component_args(ctx, comp, attrs, what):
	args = []
	for prop in comp.props:
		raw = attrs.get(prop.name, prop.default)
		if raw is None:
			raise GenError(f'{what}: missing required property {prop.name}')
		kind = prop.type
		if prop.name in attrs:
			args.append(ctx.resolve(raw, kind, what))
		else:
			# defaults are resolved in the component's own scope
			dctx = Ctx(ctx.proj, comp)
			args.append(dctx.resolve(raw, kind, what))
	return args


def gen_element(ctx, depth, parent, el):
	p = ctx.proj.p
	tag = el.tag
	attrs = dict(el.attrib)
	v = ctx.var()
	what = f'{ctx.comp.name}: <{tag}>'
	if tag in WIDGETS:
		ctx.emit(depth, f'lv_obj_t * {v} = {WIDGETS[tag]}({parent});')
		apply_attrs(ctx, depth, v, tag, attrs)
	elif tag == 'lv_tabview-tab':
		ctx.emit(depth, f'lv_obj_t * {v} = lv_tabview_add_tab({parent}, {ctx.resolve(attrs.get("text", ""), "string", what)});')
		apply_attrs(ctx, depth, v, 'lv_obj', attrs, skip=('text',))
	elif tag == 'lv_dropdown-list':
		ctx.emit(depth, f'lv_obj_t * {v} = lv_dropdown_get_list({parent});')
		apply_attrs(ctx, depth, v, 'lv_obj', attrs)
	elif tag == 'lv_tabview-tab_bar':
		ctx.emit(depth, f'lv_obj_t * {v} = lv_tabview_get_tab_bar({parent});')
		apply_attrs(ctx, depth, v, 'lv_obj', attrs)
	elif tag == 'lv_tabview-tab_button':
		ctx.emit(depth, f'lv_obj_t * {v} = lv_tabview_get_tab_button({parent}, {ctx.resolve(attrs.get("index", "0"), "int", what)});')
		apply_attrs(ctx, depth, v, 'lv_button', attrs, skip=('index',))
	elif tag in ctx.proj.components and not ctx.proj.components[tag].is_screen:
		comp = ctx.proj.components[tag]
		args = component_args(ctx, comp, attrs, what)
		ctx.emit(depth, f'lv_obj_t * {v} = {p}{tag}_create({", ".join([parent] + args)});')
		rest = {k: val for k, val in attrs.items() if k not in {pr.name for pr in comp.props}}
		apply_attrs(ctx, depth, v, ctx.proj.base_widget(tag), rest)
	else:
		raise GenError(f'{what}: unknown element')
	created = len(ctx.lines) - 1
	while not ctx.lines[created].lstrip().startswith(f'lv_obj_t * {v} = '):
		created -= 1
	gen_children(ctx, depth, v, el)
	used = re.compile(rf'\b{v}\b')
	if not any(used.search(line) for line in ctx.lines[created + 1:]):
		ctx.lines[created] = ctx.lines[created].replace(f'lv_obj_t * {v} = ', '', 1)


C_TYPES = {'string': 'const char *', 'int': 'int32_t', 'bool': 'bool', 'color': 'lv_color_t',
	   'subject': 'lv_subject_t *', 'style': 'lv_style_t *', 'font': 'const lv_font_t *', 'px': 'int32_t'}


def proto(proj, comp):
	p = proj.p
	if comp.is_screen:
		return f'lv_obj_t * {p}{comp.name}_create(void)'
	params = ['lv_obj_t * parent']
	for prop in comp.props:
		if prop.type not in C_TYPES:
			raise GenError(f'{comp.name}: unsupported property type {prop.type!r}')
		params.append(f'{C_TYPES[prop.type]} {ident(prop.name)}')
	return f'lv_obj_t * {p}{comp.name}_create({", ".join(params)})'


def gen_style_init(ctx, depth, style_el, cvar):
	ctx.emit(depth, f'lv_style_init(&{cvar});')
	for name, raw in style_el.attrib.items():
		if name in ('name', 'help'):
			continue
		ctx.emit(depth, f'lv_style_set_{name}(&{cvar}, {style_value(ctx, name, raw, "style " + style_el.get("name"))});')


def gen_component(proj, comp, out_dir, header):
	p = proj.p
	ctx = Ctx(proj, comp)
	view = comp.view
	attrs = {k: v for k, v in view.attrib.items() if k != 'extends'}
	if comp.styles:
		raise GenError(f'{comp.name}: component-local styles are not supported; put them in globals.xml')
	if comp.is_screen:
		ctx.emit(1, 'lv_obj_t * obj0 = lv_obj_create(NULL);')
		apply_attrs(ctx, 1, 'obj0', 'lv_obj', attrs)
	elif comp.extends in WIDGETS:
		ctx.emit(1, f'lv_obj_t * obj0 = {WIDGETS[comp.extends]}(parent);')
		apply_attrs(ctx, 1, 'obj0', comp.extends, attrs)
	elif comp.extends in proj.components:
		base = proj.components[comp.extends]
		args = component_args(ctx, base, attrs, f'{comp.name}: <view>')
		ctx.emit(1, f'lv_obj_t * obj0 = {p}{comp.extends}_create({", ".join(["parent"] + args)});')
		rest = {k: v for k, v in attrs.items() if k not in {pr.name for pr in base.props}}
		apply_attrs(ctx, 1, 'obj0', proj.base_widget(comp.extends), rest)
	else:
		raise GenError(f'{comp.name}: cannot extend {comp.extends!r}')
	gen_children(ctx, 1, 'obj0', view)
	ctx.emit(1, 'return obj0;')

	src = [f'/* Generated by scripts/lvgl_xml2c.py from {"screens" if comp.is_screen else "components"}/{comp.name}.xml. Do not edit. */',
	       '', f'#include "{header}"', '']
	if ctx.externs:
		src += sorted(ctx.externs) + ['']
	for fn in sorted(ctx.helpers):
		src += [ctx.helpers[fn]]
	src += [proto(proj, comp), '{'] + ctx.lines + ['}', '']
	path = os.path.join(out_dir, f'{p}{comp.name}_gen.c')
	with open(path, 'w') as f:
		f.write('\n'.join(src))
	return ctx.externs


def gen_globals(proj, out_dir, header, base):
	p = proj.p
	ctx = Ctx(proj, None)
	ctx.params = {}

	class _G:
		name = 'globals'
		consts = {}
	ctx.comp = _G
	body = []
	decls = []
	for s in proj.subjects:
		n = ident(s.get('name'))
		decls.append(f'lv_subject_t * {p}{n};')
		if s.tag == 'int':
			body.append(f'\t{p}{n} = lv_subject_create(LV_SUBJECT_TYPE_INT);')
			if s.get('min_value') is not None:
				body.append(f'\tlv_subject_set_min_value_int({p}{n}, {ctx.resolve(s.get("min_value"), "int", n)});')
			if s.get('max_value') is not None:
				body.append(f'\tlv_subject_set_max_value_int({p}{n}, {ctx.resolve(s.get("max_value"), "int", n)});')
			body.append(f'\tlv_subject_set_int({p}{n}, {ctx.resolve(s.get("value", "0"), "int", n)});')
		elif s.tag == 'string':
			decls.append(f'static char {n}_buf[{STRING_SUBJECT_SIZE}];')
			decls.append(f'static char {n}_prev[{STRING_SUBJECT_SIZE}];')
			body.append(f'\t{p}{n} = lv_subject_create(LV_SUBJECT_TYPE_STRING);')
			body.append(f'\tlv_subject_set_string_buffer_static({p}{n}, {n}_buf, {n}_prev, {STRING_SUBJECT_SIZE});')
			body.append(f'\tlv_subject_set_string({p}{n}, {ctx.resolve(s.get("value", ""), "string", n)});')
		else:
			raise GenError(f'subject {n}: unsupported type {s.tag}')
	for st in proj.styles:
		n = ident(st.get('name'))
		decls.append(f'lv_style_t {p}{n};')
		ctx.lines = []
		gen_style_init(ctx, 1, st, f'{p}{n}')
		body += ctx.lines
	screens = [c for c in proj.components.values() if c.is_screen and c.permanent]
	for c in screens:
		decls.append(f'lv_obj_t * {p}{c.name};')
	for c in screens:
		body.append(f'\t{p}{c.name} = {p}{c.name}_create();')
	src = ['/* Generated by scripts/lvgl_xml2c.py from globals.xml. Do not edit. */', '',
	       f'#include "{header}"', ''] + decls + ['', f'void {p}init_gen(void)', '{'] + body + ['}', '']
	with open(os.path.join(out_dir, f'{base}_gen.c'), 'w') as f:
		f.write('\n'.join(src))


def gen_header(proj, out_dir, header, externs):
	p = proj.p
	guard = re.sub(r'[^A-Z0-9]', '_', header.upper())
	h = [f'/* Generated by scripts/lvgl_xml2c.py. Do not edit. */', '', f'#ifndef {guard}', f'#define {guard}', '',
	     '#include <lvgl.h>', '', '#ifdef __cplusplus', 'extern "C" {', '#endif', '']
	for f in proj.fonts:
		h.append(f'LV_FONT_DECLARE({p}{f.get("name")})')
	h.append('')
	cctx = Ctx(proj, None)
	for name, (ctype, cval) in proj.consts.items():
		kind = {'color': 'color', 'px': 'int', 'int': 'int', 'opa': 'opa', 'string': 'string'}.get(ctype)
		if kind is None:
			raise GenError(f'const {name}: unsupported type {ctype}')
		h.append(f'#define {p.upper()}{ident(name).upper()} {cctx.literal(cval, kind, "const " + name)}')
	h.append('')
	for s in proj.subjects:
		h.append(f'extern lv_subject_t * {p}{s.get("name")};')
	h.append('')
	for st in proj.styles:
		h.append(f'extern lv_style_t {p}{st.get("name")};')
	h.append('')
	for c in proj.components.values():
		if c.is_screen and c.permanent:
			h.append(f'extern lv_obj_t * {p}{c.name};')
	h.append('')
	for c in proj.components.values():
		h.append(proto(proj, c) + ';')
	h += ['', '/* Event callbacks named in the XML; the application provides them. */'] + sorted(externs)
	h += ['', '/* Initialise subjects and styles, then create the permanent screens. */',
	      f'void {p}init_gen(void);', '', '#ifdef __cplusplus', '}', '#endif', '', f'#endif /* {guard} */', '']
	with open(os.path.join(out_dir, header), 'w') as f:
		f.write('\n'.join(h))


def gen_fonts(proj, out_dir, font_conv, fallback):
	p = proj.p
	fdir = os.path.join(out_dir, 'fonts')
	os.makedirs(fdir, exist_ok=True)
	for f in proj.fonts:
		if f.tag != 'bin' or f.get('as_file') != 'false':
			raise GenError(f'font {f.get("name")}: only <bin as_file="false"> fonts are supported')
		name = f'{p}{f.get("name")}'
		out = os.path.join(fdir, f'{name}.c')
		if not font_conv:
			if not os.path.exists(out):
				raise GenError(f'{out} does not exist; pass --font-conv to generate it')
			continue
		cmd = [font_conv, '--no-compress', '--no-prefilter', '--format', 'lvgl', '--lv-include', 'lvgl.h',
		       '--bpp', f.get('bpp', '4'), '--size', f.get('size'), '--lv-font-name', name,
		       '--font', os.path.join(proj.dir, f.get('src_path')), '-r', f.get('range', '0x20-0x7F'),
		       '-o', out]
		if f.get('symbols'):
			cmd[cmd.index('-r') + 2:cmd.index('-r') + 2] = ['--symbols', f.get('symbols')]
		subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
		with open(out) as fh:
			text = fh.read()
		# The converter's header records local paths; replace it so output does not depend on the cwd.
		if text.startswith('/*'):
			text = (f'/* {os.path.basename(f.get("src_path"))}, {f.get("size")} px, {f.get("bpp", "4")} bpp, '
				f'converted by lv_font_conv via scripts/lvgl_xml2c.py. */\n' + text[text.index('*/') + 3:])
		if fallback:
			if '.fallback = NULL,' not in text:
				raise GenError(f'{out}: no fallback field to set')
			text = text.replace('.fallback = NULL,', f'.fallback = &{ident(fallback)},')
		with open(out, 'w') as fh:
			fh.write(text)


def generate(src, out_dir, prefix, font_conv, fallback=None):
	proj = Project(src, prefix)
	os.makedirs(out_dir, exist_ok=True)
	base = prefix.rstrip('_') or 'ui'
	header = f'{base}_gen.h'
	externs = set()
	for comp in proj.components.values():
		externs |= gen_component(proj, comp, out_dir, header)
	gen_globals(proj, out_dir, header, base)
	gen_header(proj, out_dir, header, externs)
	gen_fonts(proj, out_dir, font_conv, fallback)


def main():
	ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
	ap.add_argument('src')
	ap.add_argument('out')
	ap.add_argument('--prefix', default='mcui_')
	ap.add_argument('--font-conv')
	ap.add_argument('--font-fallback')
	ap.add_argument('--check', action='store_true')
	a = ap.parse_args()
	try:
		if a.check:
			with tempfile.TemporaryDirectory() as tmp:
				if os.path.isdir(os.path.join(a.out, 'fonts')):
					shutil.copytree(os.path.join(a.out, 'fonts'), os.path.join(tmp, 'fonts'))
				generate(a.src, tmp, a.prefix, None)
				cmp = filecmp.dircmp(a.out, tmp)
				bad = cmp.diff_files + cmp.left_only + cmp.right_only
				if bad:
					print('generated UI code is stale: ' + ', '.join(sorted(bad)), file=sys.stderr)
					return 1
		else:
			generate(a.src, a.out, a.prefix, a.font_conv, a.font_fallback)
	except GenError as err:
		print(f'lvgl_xml2c: {err}', file=sys.stderr)
		return 1
	return 0


if __name__ == '__main__':
	sys.exit(main())
