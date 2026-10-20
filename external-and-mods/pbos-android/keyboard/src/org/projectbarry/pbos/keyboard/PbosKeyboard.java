package org.projectbarry.pbos.keyboard;

import android.inputmethodservice.InputMethodService;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.inputmethod.EditorInfo;

/**
 * The shared Android's keyboard: draws nothing. When a text field asks for a
 * keyboard it logs "show", and pbos-apk on the host opens the device's own
 * keyboard (Steam's, or Barry's on the Thor), whose keys reach Android
 * through gamescope like a hardware keyboard. It then hides itself, so the
 * next tap on a text field asks again.
 */
public class PbosKeyboard extends InputMethodService {
    private static final String TAG = "PbosKeyboard";
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    public View onCreateInputView() {
        return new View(this);
    }

    @Override
    public boolean onEvaluateFullscreenMode() {
        return false;
    }

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        super.onStartInputView(info, restarting);
        Log.i(TAG, "show");
        handler.post(() -> requestHideSelf(0));
    }
}
