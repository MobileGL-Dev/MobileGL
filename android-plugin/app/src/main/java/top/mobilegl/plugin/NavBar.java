package top.mobilegl.plugin;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Bottom navigation bar switching between the dev screens (POST and the render
 * server control). The bar only appears in flavors that ship both screens: the
 * plugin flavor has no ServerControlActivity, so wrap() returns the content
 * unchanged there. Tabs switch by reordering the other activity to the front,
 * so each screen keeps its state while the other is open.
 */
public final class NavBar {
    public static final int TAB_POST = 0;
    public static final int TAB_SERVER = 1;

    private static final int COLOR_BAR = 0xFF1E1E1E;
    private static final int COLOR_TAB_ACTIVE = 0xFFEEEEEE;
    private static final int COLOR_TAB_INACTIVE = 0xFF9E9E9E;
    private static final int BAR_HEIGHT_DP = 56;

    private static final String SERVER_ACTIVITY = "top.mobilegl.plugin.ServerControlActivity";

    private NavBar() {}

    /** Returns content wrapped with the bottom bar, or content unchanged in plugin builds. */
    public static View wrap(Activity activity, View content, int currentTab) {
        final Class<?> serverClass;
        try {
            serverClass = Class.forName(SERVER_ACTIVITY);
        } catch (ClassNotFoundException notInThisFlavor) {
            return content;
        }

        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.addView(content, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout bar = new LinearLayout(activity);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackgroundColor(COLOR_BAR);
        addTab(activity, bar, "POST", PostActivity.class, currentTab == TAB_POST);
        addTab(activity, bar, "Render Server", serverClass, currentTab == TAB_SERVER);
        root.addView(bar, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(activity, BAR_HEIGHT_DP)));
        return root;
    }

    private static void addTab(final Activity activity, LinearLayout bar, String label,
                               final Class<?> target, boolean active) {
        TextView tab = new TextView(activity);
        tab.setText(label);
        tab.setGravity(Gravity.CENTER);
        tab.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        tab.setTypeface(Typeface.MONOSPACE, active ? Typeface.BOLD : Typeface.NORMAL);
        tab.setTextColor(active ? COLOR_TAB_ACTIVE : COLOR_TAB_INACTIVE);
        if (!active) {
            tab.setOnClickListener(v -> {
                activity.startActivity(
                        new Intent(activity, target).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
                // Tab switch, not navigation: suppress the slide-in activity transition.
                activity.overridePendingTransition(0, 0);
            });
        }
        bar.addView(tab, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));
    }

    private static int dp(Activity activity, int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
