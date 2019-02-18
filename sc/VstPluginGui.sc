VstPluginGui : ObjectGui {
	// class defaults (can be overwritten per instance)
	classvar <>numRows = 10; // max. number of parameters per column
	classvar <>closeOnFree = true;
	classvar <>sliderWidth = 200;
	classvar <>sliderHeight = 20;
	classvar <>displayWidth = 60;
	classvar <>menu = true;
	// public
	var <>closeOnFree;
	var <>numRows;
	var <>sliderWidth;
	var <>sliderHeight;
	var <>displayWidth;
	var <>menu;
	// private
	classvar pluginPath;
	classvar presetPath;
	var programMenu;
	var paramSliders;
	var paramDisplays;
	var embedded;

	model_ { arg newModel;
		// always notify when changing models (but only if we have a view)
		model.removeDependant(this);
		model = newModel;
		model.addDependant(this);
		view.notNil.if {
			this.update;
		}
	}

	// this is called whenever something important in the model changes.
	update { arg who, what ...args;
		{
			(who == model).if {
				what.switch
				{ '/open'} { this.prUpdateGui }
				{ '/close' } { this.prUpdateGui }
				{ '/free' } { this.prFree } // Synth has been freed
				{ '/param' } { this.prParam(*args) }
				{ '/program' } { this.prProgram(*args) }
				{ '/program_index' } { this.prProgramIndex(*args) };
			};
			// empty update call
			who ?? { this.prUpdateGui };
		}.defer;
	}

	guify { arg parent, bounds;
		// converts the parent to a FlowView or compatible object
		// thus creating a window from nil if needed
		// registers to remove self as dependent on model if window closes
		bounds.notNil.if {
			bounds = bounds.asRect;
		};
		parent.isNil.if {
			parent = Window(bounds: bounds, scroll: true);
		} { parent = parent.asView };
		// notify the GUI on close to release its dependencies!
		parent.asView.addAction({ this.viewDidClose }, 'onClose');
		^parent
	}

	gui { arg parent, bounds;
		var layout = this.guify(parent, bounds);
		parent.isNil.if {
			view = View(layout, bounds).background_(this.background);
			embedded = false;
		} {
			view = View.new(bounds: bounds);
			ScrollView(layout, bounds)
			.background_(this.background)
			.hasHorizontalScroller_(true)
			.autohidesScrollers_(true)
			.canvas_(view);
			embedded = true;
		};
		this.prUpdateGui;
		// window
		parent.isNil.if {
			bounds.isNil.if {
				var numRows = this.numRows ?? this.class.numRows;
				var sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
				layout.setInnerExtent(sliderWidth * 2, numRows * 40);
			};
			layout.front;
		};
	}

	prUpdateGui {
		var rowOnset, nparams=0, name, header, ncolumns=0, nrows=0;
		var grid, font, minWidth, minHeight, minSize;
		var numRows = this.numRows ?? this.class.numRows;
		var sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
		var sliderHeight = this.sliderHeight ?? this.class.sliderHeight;
		var displayWidth = this.displayWidth ?? this.class.displayWidth;
		var menu = this.menu ?? this.class.menu;
		// remove old GUI body
		view !? { view.removeAll };
		(model.notNil and: { model.info.notNil}).if {
			name = model.info.name;
			menu = menu.asBoolean;
			// parameters: calculate number of rows and columns
			nparams = model.numParameters;
			ncolumns = nparams.div(numRows) + ((nparams % numRows) != 0).asInt;
			(ncolumns == 0).if {ncolumns = 1}; // just to prevent division by zero
			nrows = nparams.div(ncolumns) + ((nparams % ncolumns) != 0).asInt;
		} { menu = false };

		font = Font.new(*GUI.skin.fontSpecs).size_(14);
		// change window header
		embedded.not.if {
			view.parent.name_(name !? { "VstPlugin (%)".format(name) } ?? { "VstPlugin (empty)" });
		};

		header = HLayout(StaticText.new(view)
			.stringColor_(GUI.skin.fontColor)
			.font_(font)
			.background_(GUI.skin.background)
			.align_(\center)
			.object_(name ?? "[empty]"),
			Button.new.states_([["Open"]])
			.maxWidth_(60)
			.action_({this.prOpen})
		);

		grid = GridLayout.new;
		grid.add(header, 0, 0);
		menu.if {
			var row = 1, col = 0;
			var makePanel = { arg what;
				var label, read, write;
				label = StaticText.new.string_(what).align_(\right);
				read = Button.new.states_([["Read"]]).action_({
					var sel = ("read" ++ what).asSymbol;
					FileDialog.new({ arg path;
						presetPath = path;
						model.perform(sel, path);
					}, nil, 1, 0, true, presetPath);
				}).maxWidth_(60);
				write = Button.new.states_([["Write"]]).action_({
					var sel = ("write" ++ what).asSymbol;
					FileDialog.new({ arg path;
						presetPath = path;
						model.perform(sel, path);
					}, nil, 0, 1, true, presetPath);
				}).maxWidth_(60);
				HLayout(label, read, write);
			};
			// build program menu
			programMenu = PopUpMenu.new;
			programMenu.action = { model.program_(programMenu.value) };
			programMenu.items_(model.programNames.collect { arg item, index;
				"%: %".format(index, item);
			});
			programMenu.value_(model.program);
			grid.add(programMenu, row, col);
			// try to use another columns if available
			row = (ncolumns > 1).if { 0 } { row + 1 };
			col = (ncolumns > 1).asInt;
			grid.add(makePanel.value("Program"), row, col);
			grid.add(makePanel.value("Bank"), row + 1, col);
			rowOnset = row + 2;
		} { programMenu = nil; rowOnset = 1 };

		paramSliders = Array.new(nparams);
		paramDisplays = Array.new(nparams);

		nparams.do { arg i;
			var col, row, name, label, display, slider, bar, unit, param, labelWidth = 50;
			param = model.paramCache[i];
			col = i.div(nrows);
			row = i % nrows;
			// param name
			name = StaticText.new
			.string_("%: %".format(i, model.info.parameterNames[i]))
			.minWidth_(sliderWidth - displayWidth - labelWidth);
			// param label
			label = StaticText.new.string_(model.info.parameterLabels[i] ?? "");
			// param display
			display = TextField.new.fixedWidth_(displayWidth).string_(param[1]);
			display.action = {arg s; model.set(i, s.value)};
			paramDisplays.add(display);
			// slider
			slider = Slider.new(bounds: sliderWidth@sliderHeight)
			.fixedSize_(sliderWidth@sliderHeight).value_(param[0]);
			slider.action = {arg s; model.set(i, s.value)};
			paramSliders.add(slider);
			// put together
			bar = View.new.layout_(HLayout.new(name.align_(\left),
				nil, display.align_(\right), label.align_(\right)));
			unit = VLayout.new(bar, slider).spacing_(20);
			grid.add(unit, row+rowOnset, col);
		};
		grid.setRowStretch(nrows + rowOnset, 1);

		// add a view and make the area large enough to hold all its contents
		minWidth = ((sliderWidth + 20) * ncolumns).max(sliderWidth);
		minHeight = ((sliderHeight * 3 * nrows) + 120).max(sliderWidth); // empirically
		minSize = minWidth@minHeight;
		view.minSize_(minSize);
		View.new(view).layout_(grid).minSize_(minSize);
	}

	prParam { arg index, value, display;
		paramSliders[index].value_(value);
		paramDisplays[index].string_(display);
	}
	prProgram { arg index, name;
		var items, value;
		programMenu !? {
			value = programMenu.value;
			items = programMenu.items;
			items[index] = "%: %".format(index, name);
			programMenu.items_(items);
			programMenu.value_(value);
		};
	}
	prProgramIndex { arg index;
		programMenu !? { programMenu.value_(index)};
	}
	prFree {
		(this.closeOnFree ?? this.class.closeOnFree).if {
			embedded.not.if {
				view.parent.close;
				^this;
			};
		};
		this.prUpdateGui;
	}
	prOpen {
		model.notNil.if {
			var window, browser, file, search, path, ok, cancel, status, key, absPath;
			// build dialog
			window = Window.new.alwaysOnTop_(true).name_("VST plugin browser");
			browser = ListView.new.selectionMode_(\single).items_(VstPlugin.pluginKeys(model.synth.server));
			browser.action = {
				var info;
				key = browser.items[browser.value].asSymbol;
				info = VstPlugin.plugins(model.synth.server)[key];
				info.notNil.if {
					absPath = info.path;
					path.string = "Path:" + absPath;
				} { "bug: no info!".error; }; // should never happen
			};
			search = Button.new.states_([["Search"]]).maxWidth_(60);
			search.action = {
				status.string_("searching...");
				VstPlugin.search(model.synth.server, verbose: true, action: {
					{
						status.string_("");
						browser.items = VstPlugin.pluginKeys(model.synth.server);
						browser.value !? { browser.action.value };
					}.defer;
				});
			};
			file = Button.new.states_([["File"]]).maxWidth_(60);
			file.action = {
				FileDialog.new({ arg p;
					absPath = p;
					key = absPath;
					path.string = "Path:" + absPath;
					// do open
					ok.action.value;
				}, nil, 1, 0, true, pluginPath);
			};
			path = StaticText.new.align_(\left).string_("Path:");
			cancel = Button.new.states_([["Cancel"]]).maxWidth_(60);
			cancel.action = { window.close };
			ok = Button.new.states_([["OK"]]).maxWidth_(60);
			ok.action = {
				key !? {
					// open with key - not absPath!
					model.open(key);
					pluginPath = absPath;
					window.close;
				};
			};
			status = StaticText.new.stringColor_(Color.red).align_(\left);
			window.layout_(VLayout(
				browser, path, HLayout(search, file, status, nil, cancel, ok)
			));
			window.front;
		} { "no model!".error };
	}
	writeName {}
}
