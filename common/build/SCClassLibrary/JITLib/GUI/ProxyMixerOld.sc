// idea by adc, mods by /f0, jrh, mc, ...
		// so just call the parent window.
	
	w { 
		warn("" ++ this.class ++ ":w is deprecated. use .window instead.") 
		^window 
	}
	
		this.makeWindow;
					if (mod.notNil and: { mod.isAlt }) { 
						NodeProxyEditor(pxmon.proxy);					} { 
					};