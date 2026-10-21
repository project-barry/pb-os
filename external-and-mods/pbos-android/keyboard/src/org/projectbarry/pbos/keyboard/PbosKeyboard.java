package org.projectbarry.pbos.keyboard;

import android.inputmethodservice.InputMethodService;
import android.os.ResultReceiver;
import android.util.Log;
import android.view.View;
import android.view.inputmethod.InputMethod;

/**
 * The shared Android's keyboard. When an app or Android asks for a keyboard
 * it logs "show", and pbos-apk on the host opens the device's own keyboard
 * (Steam's, or Barry's on the Thor), whose keys reach Android through
 * gamescope as a hardware keyboard's. When they ask for it to go away (focus
 * left the text field, a search was sent) it logs "hide", and pbos-apk
 * closes that keyboard, as a phone's goes away. It never shows itself:
 * Android stays as with a hardware keyboard, and apps keep the field focused
 * (Firefox leaves the address bar when a keyboard it saw opening goes away).
 */
public class PbosKeyboard extends InputMethodService {
    private static final String TAG = "PbosKeyboard";

    @Override
    public View onCreateInputView() {
        return new View(this);
    }

    @Override
    public boolean onEvaluateFullscreenMode() {
        return false;
    }

    @Override
    public boolean onShowInputRequested(int flags, boolean configChange) {
        return false;
    }

    @Override
    public AbstractInputMethodImpl onCreateInputMethodInterface() {
        return new InputMethodImpl() {
            @Override
            public void showSoftInput(int flags, ResultReceiver resultReceiver) {
                Log.i(TAG, "show");
                super.showSoftInput(flags, resultReceiver);
            }

            @Override
            public void hideSoftInput(int flags, ResultReceiver resultReceiver) {
                Log.i(TAG, "hide");
                super.hideSoftInput(flags, resultReceiver);
            }
        };
    }
}
