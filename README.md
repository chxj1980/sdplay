# sdplay
sd card playback

# bug list
- [ ] 手机app进程杀死，如果当前正在进程sd卡录像回放，设备端感知不到手机app的退出，回放线程不会退出，直到tutk的心跳检测机制检测到手机app的连接断开，回放线程才会退出。解决：手机app在被杀死时机调用IOTCAPIs.IOTC_Session_Close，断开连接，释放资源
- [ ] 手机app从片段列表页面，返回到设备列表页面，需要调用IOTCAPIs.IOTC_Session_Close和IOTCAPIs.IOTC_Connect_Stop_BySID和AVAPIs.avClientStop去断开连接。否则下一次再连接设备端，上一次的session并没有释放掉。
- [ ] 从倍速播放页面，返回到片段列表页面，有的时候收不到stop信令
