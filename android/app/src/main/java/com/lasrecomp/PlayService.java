package com.lasrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

// While the app is open, a foreground service: the game takes a few GB, so a trip to the home screen made the process
// the first one Android (and some makers' background app killers) ended, and coming back started over at the start
// screen. As a foreground service the process stays "perceptible" and the game waits paused where it was (nothing
// runs in the background). Started by LASRecompActivity whenever it's in front (only a visible app may start one),
// ended with it. Nothing runs in here: it only keeps the process alive.
// From Nycolazs's LASRecomp-Android-Plus via mamcjr's LASRecompAndroid (both MIT).
public class PlayService extends Service {
    private static final String CHANNEL = "play";
    private static final int ID = 4201;

    static void start(Context c) {
        try {
            c.startForegroundService(new Intent(c, PlayService.class));
        } catch (Exception e) {  // (not allowed right now: the game runs on, only less protected in the background)
            Log.w("LASRecomp", "play service: " + e);
        }
    }

    static void stop(Context c) {
        c.stopService(new Intent(c, PlayService.class));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationChannel ch = new NotificationChannel(CHANNEL, "Game session", NotificationManager.IMPORTANCE_MIN);
        ch.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(ch);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Intent open = new Intent(this, LASRecompActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(this, 1, open, PendingIntent.FLAG_IMMUTABLE);
        Notification n = new Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_media_pause)
            .setContentTitle("LAS Recomp is open")
            .setContentText("The game waits paused where you left it.")
            .setOngoing(true)
            .setContentIntent(pi)
            .build();
        try {
            if (Build.VERSION.SDK_INT >= 34) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else startForeground(ID, n);
        } catch (Exception e) {
            Log.w("LASRecomp", "play service foreground: " + e);
            stopSelf();
        }
        return START_NOT_STICKY;  // (the game lives in the app's process: nothing to restart without it)
    }

    @Override
    public void onDestroy() {
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
