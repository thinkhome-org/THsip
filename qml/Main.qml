import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtMultimedia
import QtWebEngine

ApplicationWindow {
    id: root
    width: 1180
    height: 760
    visible: true
    title: "THsip"
    color: palette.window

    property var nav: [
        { label: "Přehled", page: 0 }, { label: "Volání", page: 1 },
        { label: "Odorik API", page: 2 }, { label: "SMS", page: 3 },
        { label: "SIM", page: 4 }, { label: "Kontakty", page: 5 },
        { label: "Routing", page: 6 }, { label: "Nahrávky", page: 7 },
        { label: "Funkce", page: 8 }, { label: "Bridge", page: 9 },
        { label: "Diagnostika", page: 10 }, { label: "Portál", page: 11 },
        { label: "Nastavení", page: 12 }
    ]
    property string sipAccountId: ""
    property string currentCallId: ""
    property string consultationCallId: ""
    property string currentCallState: "Bez hovoru"
    property string buddyId: ""
    property string buddyStateText: "Nesledováno"
    property string messageLog: ""
    property bool muted: false
    property string recordingPath: ""
    property var mediaStats: []
    property var audioDeviceList: []
    property var videoDeviceList: []
    property var codecList: []
    property string exportRecordingId: ""
    property var telephonyDiagnostics: ({})
    property var diagnosticEvents: []
    property var systemDiagnosticData: ({})

    function money(value) { let number = Number(value); return isNaN(number) ? "—" : number.toLocaleString(Qt.locale("cs_CZ"), "f", 2) + " Kč" }
    function duration(value) {
        let seconds = Number(value)
        if (isNaN(seconds)) return "—"
        return Math.floor(seconds / 3600) + " h " + Math.floor((seconds % 3600) / 60) + " min"
    }
    function periodValue(period, key) {
        let value = backend.dashboard[period]
        return value ? value[key] || 0 : 0
    }
    function registeredLines() {
        let count = 0
        let lines = backend.dashboard.lines || []
        for (let line of lines) if ((line.connected_devices || []).length > 0) ++count
        return count
    }

    FileDialog {
        id: contactFile
        title: "Import kontaktů"
        nameFilters: ["Kontakty (*.csv *.vcf)"]
        onAccepted: backend.importContacts(selectedFile)
    }
    FileDialog {
        id: recordingExport
        title: "Export nahrávky"
        fileMode: FileDialog.SaveFile
        nameFilters: ["Audio (*.wav *.mp3 *.ogg *.opus)"]
        onAccepted: backend.exportRecording(root.exportRecordingId, selectedFile)
    }
    AudioOutput { id: recordingAudio }
    MediaPlayer {
        id: recordingPlayer
        source: backend.recordingPlaybackUrl
        audioOutput: recordingAudio
        onSourceChanged: if (source) play()
    }
    SoundEffect { id: testTone; source: backend.testToneUrl() }
    Camera { id: diagnosticCamera; active: false }
    CaptureSession { camera: diagnosticCamera; videoOutput: diagnosticVideo }

    component ApiPanel: Pane {
        id: apiPanel
        required property string panelTitle
        property string category: ""
        property var recipes: category ? backend.apiRecipes.filter(recipe => recipe.category === category) : backend.apiRecipes
        property var currentRecipe: ({})
        property var pendingParameters: ({})

        function choose(index) {
            if (index < 0 || index >= recipes.length) return
            currentRecipe = recipes[index]
            apiMethod.text = currentRecipe.method
            apiPath.text = currentRecipe.path
            apiParameters.text = JSON.stringify(currentRecipe.parameters || {}, null, 2)
            apiError.text = ""
        }
        function send(confirmed) {
            let value = {}
            try { value = apiParameters.text.trim() ? JSON.parse(apiParameters.text) : {} }
            catch (error) { apiError.text = "Neplatné JSON parametry: " + error; return }
            if (backend.isSensitiveMutation(apiMethod.text, apiPath.text) && !confirmed) {
                pendingParameters = value
                confirmMutation.open()
                return
            }
            if (confirmed) value._confirmed = true
            backend.api(apiMethod.text, apiPath.text, value)
        }

        Component.onCompleted: { choose(0); if (category === "SMS") backend.refreshSms({}) }

        Dialog {
            id: confirmMutation
            title: "Potvrdit citlivou operaci"
            modal: true
            width: 480
            standardButtons: Dialog.Ok | Dialog.Cancel
            Label { width: 440; text: apiMethod.text + " /api/v1/" + apiPath.text; wrapMode: Text.WordWrap }
            onAccepted: { let value = Object.assign({}, apiPanel.pendingParameters); value._confirmed = true; backend.api(apiMethod.text, apiPath.text, value) }
        }

        ColumnLayout {
            anchors.fill: parent
            Label { text: apiPanel.panelTitle; font.pixelSize: 28; font.bold: true }
            Frame {
                visible: apiPanel.category === "SMS"
                Layout.fillWidth: true
                ColumnLayout {
                    id: smsComposer
                    anchors.fill: parent
                    Label { text: "SMS composer"; font.pixelSize: 20; font.bold: true }
                    RowLayout {
                        TextField { id: smsRecipient; placeholderText: "Příjemce"; Layout.fillWidth: true }
                        ComboBox { id: smsSender; editable: true; model: backend.smsSenders; Layout.fillWidth: true; displayText: editText || currentText || "Odesílatel (volitelné)" }
                        TextField { id: smsDelayed; placeholderText: "Odložit: minuty nebo čas"; Layout.fillWidth: true }
                    }
                    TextArea { id: smsMessage; placeholderText: "Text, maximálně 765 znaků"; Layout.fillWidth: true; Layout.preferredHeight: 110; wrapMode: TextEdit.Wrap }
                    property var info: backend.smsInfo(smsMessage.text)
                    RowLayout {
                        Label { text: smsComposer.info.encoding + " · " + smsComposer.info.units + " jednotek · " + smsComposer.info.segments + " segmentů · zbývá " + smsComposer.info.remaining; color: smsComposer.info.valid ? palette.text : "#b00020" }
                        Item { Layout.fillWidth: true }
                        Button { text: "Odeslat SMS"; enabled: smsComposer.info.valid && !!smsRecipient.text && !!smsMessage.text; onClicked: backend.api("POST", "sms", {recipient:smsRecipient.text, message:smsMessage.text, sender:smsSender.editText || smsSender.currentText, delayed:smsDelayed.text}) }
                    }
                    RowLayout {
                        TextField { id: smsTemplateName; placeholderText: "Název template"; Layout.fillWidth: true }
                        ComboBox {
                            id: smsTemplate; model: backend.smsTemplates; textRole: "name"; valueRole: "id"; Layout.fillWidth: true
                            onActivated: if (currentIndex >= 0) { smsTemplateName.text = backend.smsTemplates[currentIndex].name; smsMessage.text = backend.smsTemplates[currentIndex].body }
                        }
                        Button { text: "Uložit template"; onClicked: backend.saveSmsTemplate(smsTemplateName.text, smsMessage.text) }
                        Button { text: "Smazat template"; enabled: smsTemplate.currentIndex >= 0; onClicked: backend.deleteSmsTemplate(smsTemplate.currentValue) }
                    }
                }
            }
            Frame {
                visible: apiPanel.category === "SMS"
                Layout.fillWidth: true; Layout.preferredHeight: 220
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "SMS historie"; font.pixelSize: 18; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button { text: "Obnovit"; onClicked: backend.refreshSms({}) }
                    }
                    Label { text: "Odorik vrací jen metadata. Text viditelný pouze u SMS odeslaných z THsip."; opacity: .65 }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: backend.smsRecords
                        delegate: RowLayout {
                            required property var modelData; width: ListView.view.width
                            Label { text: modelData.date || modelData.time || modelData.occurred_at || "—"; Layout.preferredWidth: 165 }
                            Label { text: modelData.direction || "—"; Layout.preferredWidth: 55; font.bold: true }
                            Label { text: (modelData.source_number || modelData.sender || "—") + "  →  " + (modelData.destination_number || modelData.recipient || "—"); Layout.preferredWidth: 260; elide: Text.ElideMiddle }
                            Label { text: modelData.message || "Text není v API"; Layout.fillWidth: true; opacity: modelData.local ? 1 : .55; elide: Text.ElideRight }
                            Label { text: modelData.price !== undefined ? root.money(modelData.price) : "" }
                        }
                    }
                }
            }
            RowLayout {
                ComboBox { id: apiRecipe; model: apiPanel.recipes; textRole: "title"; Layout.fillWidth: true; onActivated: apiPanel.choose(currentIndex) }
                Label { id: apiMethod; font.bold: true }
                TextField { id: apiPath; Layout.fillWidth: true; placeholderText: "Nahraďte {parametry} v cestě" }
                Button { text: "Odeslat"; onClicked: apiPanel.send(false) }
            }
            Label { text: "Parametry JSON · prázdné hodnoty se neposílají" }
            TextArea { id: apiParameters; text: "{}"; Layout.fillWidth: true; Layout.preferredHeight: 190; font.family: "Menlo" }
            Label { id: apiError; color: "#b00020"; visible: !!text }
            Label { text: "Odpověď" }
            TextArea { text: backend.output; readOnly: true; Layout.fillWidth: true; Layout.fillHeight: true; font.family: "Menlo"; wrapMode: TextEdit.WrapAnywhere }
        }
    }

    Connections {
        target: typeof telephony !== "undefined" ? telephony : null
        function onIncomingCall(callId, state) {
            root.currentCallId = callId
            root.currentCallState = "Příchozí: " + (state.remoteUri || "")
        }
        function onCallState(callId, state) {
            if (callId === root.currentCallId || callId === root.consultationCallId)
                root.currentCallState = state.state + " · " + (state.remoteUri || "")
            if (state.state === "DISCONNECTED") {
                if (callId === root.currentCallId) root.currentCallId = ""
                if (callId === root.consultationCallId) root.consultationCallId = ""
            }
        }
        function onMessageReceived(accountId, from, contentType, message) {
            root.messageLog += (root.messageLog ? "\n" : "") + from + ": " + message
        }
        function onMessageStatus(accountId, destination, code, reason) {
            root.messageLog += "\n[" + code + " " + reason + "] " + destination
        }
        function onBuddyState(buddyId, state) {
            if (buddyId === root.buddyId)
                root.buddyStateText = state.dialogState || state.status || state.subscription || "Neznámý"
        }
        function onRecordingState(callId, recording, path) {
            if (callId === root.currentCallId) root.recordingPath = recording ? path : ""
        }
        function onDiagnosticEvent(event) {
            root.diagnosticEvents = [event].concat(root.diagnosticEvents).slice(0, 500)
            root.telephonyDiagnostics = telephony.diagnostics()
        }
        function onAccountState(accountId, registered, reason) {
            root.currentCallState = (registered ? "SIP registrován" : "SIP odpojen") + (reason ? " · " + reason : "")
        }
        function onError(operation, message) { root.currentCallState = operation + ": " + message }
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14; anchors.rightMargin: 14
            Label { text: "THsip"; font.pixelSize: 22; font.bold: true }
            Label { text: backend.pjsipAvailable ? "SIP připraven" : "PJSIP chybí"; color: backend.pjsipAvailable ? "#2e7d32" : "#b26a00" }
            Item { Layout.fillWidth: true }
            Label { text: backend.status; elide: Text.ElideRight; Layout.maximumWidth: 500 }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.fillHeight: true
            Layout.preferredWidth: 180
            background: Rectangle { color: "#17191d" }
            ColumnLayout {
                anchors.fill: parent
                Repeater {
                    model: root.nav
                    Button {
                        required property var modelData
                        text: modelData.label
                        flat: true
                        Layout.fillWidth: true
                        highlighted: pages.currentIndex === modelData.page
                        onClicked: pages.currentIndex = modelData.page
                    }
                }
                Item { Layout.fillHeight: true }
                Label { text: "Odorik.cz\nbez reverse engineeringu"; color: "#9ca3af"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }

        StackLayout {
            id: pages
            Layout.fillWidth: true
            Layout.fillHeight: true

            ScrollView {
                ColumnLayout {
                    width: Math.max(700, parent.width)
                    spacing: 14
                    RowLayout {
                        Label { text: "Přehled"; font.pixelSize: 28; font.bold: true }
                        Item { Layout.fillWidth: true }
                        BusyIndicator { running: backend.dashboard.loading || false; visible: running; implicitWidth: 30; implicitHeight: 30 }
                        Label { text: backend.dashboard.refreshedAt ? "Aktualizováno " + backend.dashboard.refreshedAt : ""; opacity: .65 }
                        Button { text: "Obnovit"; enabled: !backend.dashboard.loading; onClicked: backend.refreshDashboard() }
                    }
                    GridLayout {
                        Layout.fillWidth: true; columns: 4
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Kredit"; opacity: .7 } Label { text: backend.balance; font.pixelSize: 25; font.bold: true } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Dnes"; opacity: .7 } Label { text: root.money(root.periodValue("today", "price")); font.pixelSize: 25; font.bold: true } Label { text: root.periodValue("today", "count") + " hovorů · " + root.duration(root.periodValue("today", "length")) } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Tento týden"; opacity: .7 } Label { text: root.money(root.periodValue("week", "price")); font.pixelSize: 25; font.bold: true } Label { text: root.periodValue("week", "count") + " hovorů · " + root.duration(root.periodValue("week", "length")) } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Tento měsíc"; opacity: .7 } Label { text: root.money(root.periodValue("month", "price")); font.pixelSize: 25; font.bold: true } Label { text: root.periodValue("month", "count") + " hovorů · " + root.duration(root.periodValue("month", "length")) } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Aktivní hovory"; opacity: .7 } Label { text: (backend.dashboard.activeCalls || []).length; font.pixelSize: 25; font.bold: true } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Zmeškané cíle"; opacity: .7 } Label { text: (backend.dashboard.missed || []).length; font.pixelSize: 25; font.bold: true } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Registrované linky"; opacity: .7 } Label { text: root.registeredLines() + " / " + (backend.dashboard.lines || []).length; font.pixelSize: 25; font.bold: true } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Odorik SIM"; opacity: .7 } Label { text: (backend.dashboard.simCards || []).length; font.pixelSize: 25; font.bold: true } } }
                    }
                    Label { text: "Rychlé akce"; font.pixelSize: 20; font.bold: true }
                    RowLayout {
                        Button { text: "Zavolat"; onClicked: pages.currentIndex = 1 }
                        Button { text: "SMS"; onClicked: pages.currentIndex = 3 }
                        Button { text: "Callback"; onClicked: pages.currentIndex = 2 }
                        Button { text: "Routing"; onClicked: pages.currentIndex = 6 }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.alignment: Qt.AlignTop
                        Frame {
                            Layout.fillWidth: true; Layout.alignment: Qt.AlignTop
                            ColumnLayout {
                                anchors.fill: parent
                                Label { text: "Náklady podle linek · měsíc"; font.pixelSize: 18; font.bold: true }
                                Repeater {
                                    model: backend.dashboard.byLine || []
                                    RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Label { text: modelData.line; Layout.fillWidth: true }
                                        Label { text: modelData.count + " hovorů"; opacity: .7 }
                                        Label { text: root.money(modelData.price); font.bold: true }
                                    }
                                }
                                Label { visible: !(backend.dashboard.byLine || []).length; text: "Bez dat"; opacity: .6 }
                            }
                        }
                        Frame {
                            Layout.fillWidth: true; Layout.alignment: Qt.AlignTop
                            ColumnLayout {
                                anchors.fill: parent
                                Label { text: "Destinace · měsíc"; font.pixelSize: 18; font.bold: true }
                                Repeater {
                                    model: (backend.dashboard.destinations || []).slice(0, 8)
                                    RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Label { text: modelData.destination || "—"; Layout.fillWidth: true; elide: Text.ElideRight }
                                        Label { text: modelData.count + "×"; opacity: .7 }
                                        Label { text: root.money(modelData.price); font.bold: true }
                                    }
                                }
                                Label { visible: !(backend.dashboard.destinations || []).length; text: "Bez dat"; opacity: .6 }
                            }
                        }
                    }
                    Frame {
                        Layout.fillWidth: true
                        ColumnLayout {
                            anchors.fill: parent
                            Label { text: "SIM stav"; font.pixelSize: 18; font.bold: true }
                            Repeater {
                                model: backend.dashboard.simCards || []
                                RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Label { text: modelData.phone_number || modelData.line || "SIM"; Layout.fillWidth: true }
                                    Label { text: modelData.state || "—"; font.bold: true }
                                    Label { text: modelData.changes_in_progress ? "změna probíhá" : ""; opacity: .7 }
                                }
                            }
                            Label { visible: !(backend.dashboard.simCards || []).length; text: "Žádná SIM"; opacity: .6 }
                        }
                    }
                }
            }

            Pane {
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: "Dialer"; font.pixelSize: 28; font.bold: true }
                    TextField { id: destination; placeholderText: "Číslo, linka nebo SIP URI"; Layout.fillWidth: true; font.pixelSize: 22 }
                    Flow {
                        Layout.fillWidth: true; spacing: 8
                        CheckBox { id: record; text: "Serverové nahrávání" }
                        CheckBox { id: anonymous; text: "Anonymně" }
                        CheckBox { id: autoAnswer; text: "Early-media auto-answer" }
                        CheckBox { id: jitter; text: "Jitter 300 ms" }
                    }
                    Label { text: "Výsledný Odorik dial string"; opacity: .7 }
                    TextField {
                        id: compiled
                        Layout.fillWidth: true; readOnly: true
                        text: backend.compileDial(destination.text, [
                            ...(record.checked ? [{id:"record"}] : []),
                            ...(anonymous.checked ? [{id:"anonymous"}] : []),
                            ...(autoAnswer.checked ? [{id:"autoAnswer"}] : []),
                            ...(jitter.checked ? [{id:"jitter", parameters:{milliseconds:300}}] : [])
                        ])
                    }
                    RowLayout {
                        Button { text: "Audio hovor"; enabled: backend.pjsipAvailable && root.sipAccountId && !compiled.text.startsWith("Chyba"); onClicked: root.currentCallId = telephony.makeCall(root.sipAccountId, compiled.text, false) }
                        Button { text: "Videohovor"; enabled: backend.pjsipAvailable && root.sipAccountId && !compiled.text.startsWith("Chyba"); onClicked: root.currentCallId = telephony.makeCall(root.sipAccountId, compiled.text, true) }
                        Button { text: "Callback přes API"; onClicked: pages.currentIndex = 2 }
                    }
                    Label { text: root.currentCallState; font.bold: true }
                    RowLayout {
                        Button { text: "Přijmout"; enabled: !!root.currentCallId; onClicked: telephony.answer(root.currentCallId) }
                        Button { text: "Podržet"; enabled: !!root.currentCallId; onClicked: telephony.hold(root.currentCallId, true) }
                        Button { text: "Pokračovat"; enabled: !!root.currentCallId; onClicked: telephony.hold(root.currentCallId, false) }
                        Button { text: root.muted ? "Zapnout mikrofon" : "Mute"; enabled: !!root.currentCallId; onClicked: { root.muted = !root.muted; telephony.mute(root.currentCallId, root.muted) } }
                        Button { text: "Zavěsit"; enabled: !!root.currentCallId; onClicked: telephony.hangup(root.currentCallId) }
                    }
                    RowLayout {
                        TextField { id: transferTarget; placeholderText: "Cíl transferu"; Layout.fillWidth: true }
                        Button { text: "Blind transfer"; enabled: !!root.currentCallId && transferTarget.text; onClicked: telephony.transfer(root.currentCallId, transferTarget.text) }
                        Button { text: "Konzultace"; enabled: !!root.sipAccountId && transferTarget.text; onClicked: root.consultationCallId = telephony.makeCall(root.sipAccountId, transferTarget.text, false) }
                        Button { text: "Attended transfer"; enabled: !!root.currentCallId && !!root.consultationCallId; onClicked: telephony.attendedTransfer(root.currentCallId, root.consultationCallId) }
                        Button { text: "Konference"; enabled: !!root.currentCallId && !!root.consultationCallId; onClicked: telephony.conference(root.currentCallId, root.consultationCallId, true) }
                    }
                    RowLayout {
                        TextField { id: dtmf; placeholderText: "DTMF"; Layout.fillWidth: true }
                        Button { text: "Odeslat DTMF"; enabled: !!root.currentCallId && dtmf.text; onClicked: telephony.sendDtmf(root.currentCallId, dtmf.text) }
                    }
                    RowLayout {
                        Button { text: root.recordingPath ? "Zastavit lokální recording" : "Lokální recording"; enabled: !!root.currentCallId; onClicked: root.recordingPath ? telephony.stopRecording(root.currentCallId) : root.recordingPath = telephony.startRecording(root.currentCallId) }
                        Button { text: "Media statistics"; enabled: !!root.currentCallId; onClicked: root.mediaStats = telephony.mediaStatistics(root.currentCallId) }
                        Label { text: root.recordingPath; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    }
                    Repeater {
                        model: root.mediaStats
                        Label { required property var modelData; text: modelData.codec + " · " + modelData.bitrate.toFixed(0) + " bit/s · jitter " + modelData.jitterMs.toFixed(1) + " ms · loss " + modelData.lossPercent.toFixed(2) + "% · RTT " + modelData.rttMs.toFixed(1) + " ms · MOS " + modelData.mos.toFixed(2) }
                    }
                    Frame {
                        Layout.fillWidth: true
                        ColumnLayout {
                            anchors.fill: parent
                            Label { text: "SIP MESSAGE a BLF"; font.pixelSize: 20; font.bold: true }
                            RowLayout {
                                TextField { id: messageTarget; text: destination.text; placeholderText: "Šestimístná linka nebo SIP URI"; Layout.fillWidth: true }
                                Button { text: "Sledovat BLF"; enabled: !!root.sipAccountId && messageTarget.text; onClicked: root.buddyId = telephony.subscribeBuddy(root.sipAccountId, messageTarget.text, true) }
                                Label { text: root.buddyStateText }
                            }
                            TextArea { id: messageBody; placeholderText: "SIP zpráva"; Layout.fillWidth: true; Layout.preferredHeight: 70 }
                            RowLayout {
                                Button { text: "Odeslat"; enabled: !!root.sipAccountId && messageTarget.text && messageBody.text; onClicked: { telephony.sendMessage(root.sipAccountId, messageTarget.text, messageBody.text); messageBody.clear() } }
                                Button { text: "Píšu"; enabled: !!root.sipAccountId && messageTarget.text; onPressed: telephony.sendTyping(root.sipAccountId, messageTarget.text, true); onReleased: telephony.sendTyping(root.sipAccountId, messageTarget.text, false) }
                            }
                            TextArea { text: root.messageLog; readOnly: true; Layout.fillWidth: true; Layout.preferredHeight: 100 }
                        }
                    }
                    Label { visible: !backend.pjsipAvailable; color: "#b26a00"; text: "PJSIP 2.17 není v toolchainu. Dial compiler funguje; call control se aktivuje po linkování PJSUA2."; wrapMode: Text.WordWrap }
                    Item { Layout.fillHeight: true }
                }
            }

            Pane {
                id: callsPage
                property string pendingActiveId: ""
                function value(call, keys) {
                    for (let key of keys) if (call[key] !== undefined && call[key] !== null && call[key] !== "") return call[key]
                    return "—"
                }
                function filters(page) {
                    return {from:callsFrom.text, to:callsTo.text, direction:callsDirection.currentValue || "",
                            status:callsStatus.currentValue || "", line:callsLine.text,
                            phone_number_filter:callsPhone.text, page_size:200, page:page}
                }
                function refresh(page) { backend.refreshCalls(filters(page || 1)) }
                Component.onCompleted: refresh(1)
                Dialog {
                    id: confirmActiveHangup
                    title: "Vzdáleně ukončit aktivní hovor?"
                    modal: true; standardButtons: Dialog.Ok | Dialog.Cancel
                    Label { text: "Odorik active-call ID: " + callsPage.pendingActiveId; wrapMode: Text.WrapAnywhere }
                    onAccepted: backend.hangupActiveCall(callsPage.pendingActiveId, true)
                }
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "Hovory"; font.pixelSize: 28; font.bold: true }
                        Item { Layout.fillWidth: true }
                        TabBar {
                            id: callsTabs
                            TabButton { text: "Historie" }
                            TabButton { text: "Kompletní REST API" }
                        }
                    }
                    StackLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: callsTabs.currentIndex
                        Pane {
                            ColumnLayout {
                                anchors.fill: parent
                                RowLayout {
                                    TextField { id: callsFrom; placeholderText: "Od (ISO datum/čas)"; Layout.fillWidth: true }
                                    TextField { id: callsTo; placeholderText: "Do (ISO datum/čas)"; Layout.fillWidth: true }
                                    ComboBox { id: callsDirection; model: [{label:"Všechny směry",id:""},{label:"Příchozí",id:"in"},{label:"Odchozí",id:"out"},{label:"Přesměrované",id:"redirected"}]; textRole:"label"; valueRole:"id" }
                                    ComboBox { id: callsStatus; model: [{label:"Všechny stavy",id:""},{label:"Přijaté",id:"answered"},{label:"Zmeškané",id:"missed"}]; textRole:"label"; valueRole:"id" }
                                }
                                RowLayout {
                                    TextField { id: callsLine; placeholderText: "Linka"; Layout.fillWidth: true }
                                    TextField { id: callsPhone; placeholderText: "Telefonní číslo"; Layout.fillWidth: true }
                                    Button { text: "Načíst"; onClicked: callsPage.refresh(1) }
                                }
                                Frame {
                                    Layout.fillWidth: true; visible: backend.activeCalls.length > 0
                                    ColumnLayout {
                                        anchors.fill: parent
                                        Label { text: "Aktivní hovory"; font.pixelSize: 18; font.bold: true }
                                        Repeater {
                                            model: backend.activeCalls
                                            RowLayout {
                                                required property var modelData
                                                Layout.fillWidth: true
                                                Label { text: callsPage.value(modelData, ["from", "source_number", "caller"]) + "  →  " + callsPage.value(modelData, ["to", "destination_number", "recipient"]); Layout.fillWidth: true }
                                                Label { text: "ID " + callsPage.value(modelData, ["id"]); opacity: .65 }
                                                Button { text: "Vzdáleně zavěsit"; onClicked: { callsPage.pendingActiveId = String(callsPage.value(modelData, ["id"])); confirmActiveHangup.open() } }
                                            }
                                        }
                                    }
                                }
                                ListView {
                                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4
                                    model: backend.calls
                                    delegate: Frame {
                                        id: callRow
                                        required property var modelData
                                        width: ListView.view.width
                                        RowLayout {
                                            anchors.fill: parent
                                            Label { text: callsPage.value(callRow.modelData, ["date", "time", "start", "occurred_at"]); Layout.preferredWidth: 170 }
                                            Label { text: callsPage.value(callRow.modelData, ["direction"]); Layout.preferredWidth: 90; font.bold: true }
                                            Label { text: callsPage.value(callRow.modelData, ["source_number", "from"]) + "  →  " + callsPage.value(callRow.modelData, ["destination_number", "to"]); Layout.fillWidth: true; elide: Text.ElideMiddle }
                                            Label { text: root.duration(callsPage.value(callRow.modelData, ["length", "duration"])); Layout.preferredWidth: 100 }
                                            Label { text: root.money(callsPage.value(callRow.modelData, ["price"])); Layout.preferredWidth: 100; horizontalAlignment: Text.AlignRight }
                                            Button { text: "Volat"; onClicked: { destination.text = String(callsPage.value(callRow.modelData, ["destination_number", "to", "source_number", "from"])); pages.currentIndex = 1 } }
                                        }
                                    }
                                }
                                RowLayout {
                                    Button { text: "Předchozí"; enabled: backend.callPage > 1; onClicked: callsPage.refresh(backend.callPage - 1) }
                                    Label { text: "Strana " + backend.callPage + " / " + backend.callPages }
                                    Button { text: "Další"; enabled: backend.callPage < backend.callPages; onClicked: callsPage.refresh(backend.callPage + 1) }
                                    Item { Layout.fillWidth: true }
                                    Label { text: backend.calls.length + " záznamů"; opacity: .65 }
                                }
                            }
                        }
                        ApiPanel { panelTitle: "Kompletní Odorik REST API" }
                    }
                }
            }
            ApiPanel { panelTitle: "SMS"; category: "SMS" }
            Pane {
                id: simPage
                property var pendingChanges: ({})
                function mobile() { return simMobile.text.trim() }
                function optional(combo) { return combo.currentValue || "" }
                function changes() {
                    return {state:optional(simState), data_package:simDataPackage.text, data_package_for_next_month:simDataPackageNext.text,
                        voice_package:simVoicePackage.text, voice_package_for_next_month:simVoicePackageNext.text,
                        package_delayed_billing:simDelayedPackage.text, package_delayed_billing_for_next_month:simDelayedPackageNext.text,
                        missed_calls_register:optional(simMissed), mobile_data:optional(simMobileData), lte:optional(simLte),
                        lte_for_next_month:optional(simLteNext), roaming:simRoaming.text,
                        premium_services:optional(simPremium), add_credit:simAddCredit.text}
                }
                Component.onCompleted: backend.refreshSimCards()
                Dialog {
                    id: confirmSimUpdate; title: "Potvrdit změnu SIM"; modal: true; standardButtons: Dialog.Ok | Dialog.Cancel
                    TextArea { width: 520; readOnly: true; text: JSON.stringify(simPage.pendingChanges, null, 2); wrapMode: Text.WrapAnywhere }
                    onAccepted: backend.updateSim(simPage.mobile(), simPage.pendingChanges, true)
                }
                Dialog {
                    id: confirmSimRestart; title: "Restartovat/FUP mobilní data?"; modal: true; standardButtons: Dialog.Ok | Dialog.Cancel
                    Label { text: "SIM " + simPage.mobile() }
                    onAccepted: backend.restartSimData(simPage.mobile(), true)
                }
                Dialog {
                    id: assignSimDialog; title: "Přiřadit fyzickou SIM?"; modal: true; standardButtons: Dialog.Ok | Dialog.Cancel
                    ColumnLayout {
                        width: 480
                        Label { text: "Citlivá operace. PIN nebude uložen."; wrapMode: Text.WordWrap }
                        TextField { id: simIccid; placeholderText: "ICCID (18–22 číslic)"; Layout.fillWidth: true }
                        TextField { id: simPin; placeholderText: "PIN (4–8 číslic)"; echoMode: TextInput.Password; Layout.fillWidth: true }
                        CheckBox { id: simDelayedActivation; text: "Odložená aktivace" }
                    }
                    onAccepted: { backend.assignSim(simPage.mobile(), simIccid.text, simPin.text, simDelayedActivation.checked, true); simPin.clear() }
                    onRejected: simPin.clear()
                }
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "Odorik SIM"; font.pixelSize: 28; font.bold: true }
                        Item { Layout.fillWidth: true }
                        TabBar {
                            id: simTabs
                            TabButton { text: "Správa" }
                            TabButton { text: "Kompletní REST API" }
                        }
                    }
                    StackLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: simTabs.currentIndex
                        ScrollView {
                            ColumnLayout {
                                width: Math.max(720, parent.width); spacing: 10
                                RowLayout {
                                    ComboBox {
                                        id: simPicker; model: backend.simCards; textRole: "phone_number"; Layout.fillWidth: true
                                        onActivated: { let card = backend.simCards[currentIndex]; simMobile.text = String(card.phone_number || card.line || ""); backend.loadSim(simMobile.text) }
                                    }
                                    TextField { id: simMobile; placeholderText: "Mobilní číslo"; Layout.fillWidth: true }
                                    Button { text: "Načíst"; onClicked: { backend.loadSim(simPage.mobile()); backend.loadSimData(simPage.mobile()) } }
                                    Button { text: "Obnovit seznam"; onClicked: backend.refreshSimCards() }
                                }
                                Frame {
                                    Layout.fillWidth: true
                                    ColumnLayout {
                                        anchors.fill: parent
                                        Label { text: "Stav a probíhající změny"; font.pixelSize: 18; font.bold: true }
                                        TextArea { Layout.fillWidth: true; Layout.preferredHeight: 110; readOnly: true; font.family: "Menlo"; text: JSON.stringify(backend.simDetail, null, 2); wrapMode: Text.WrapAnywhere }
                                    }
                                }
                                Label { text: "Změna nastavení"; font.pixelSize: 18; font.bold: true }
                                GridLayout {
                                    Layout.fillWidth: true; columns: 4
                                    Label { text: "Stav" }
                                    ComboBox { id: simState; model: [{label:"Bez změny",id:""},{label:"Aktivní",id:"active"},{label:"Pozastavená",id:"suspended"}]; textRole:"label"; valueRole:"id"; Layout.fillWidth: true }
                                    Label { text: "Dokoupit kredit/data" }
                                    TextField { id: simAddCredit; placeholderText: "ID doplňku"; Layout.fillWidth: true }
                                    Label { text: "Datový balíček nyní" }
                                    TextField { id: simDataPackage; placeholderText: "ID / prázdné = bez změny"; Layout.fillWidth: true }
                                    Label { text: "Datový balíček příští měsíc" }
                                    TextField { id: simDataPackageNext; placeholderText: "ID"; Layout.fillWidth: true }
                                    Label { text: "Hlasový balíček nyní" }
                                    TextField { id: simVoicePackage; placeholderText: "ID"; Layout.fillWidth: true }
                                    Label { text: "Hlasový balíček příští měsíc" }
                                    TextField { id: simVoicePackageNext; placeholderText: "ID"; Layout.fillWidth: true }
                                    Label { text: "Delayed-billing balíček" }
                                    TextField { id: simDelayedPackage; placeholderText: "ID"; Layout.fillWidth: true }
                                    Label { text: "Delayed-billing příští měsíc" }
                                    TextField { id: simDelayedPackageNext; placeholderText: "ID"; Layout.fillWidth: true }
                                    Label { text: "Mobilní data" }
                                    ComboBox { id: simMobileData; model: [{label:"Bez změny",id:""},{label:"Zapnout",id:"true"},{label:"Vypnout",id:"false"}]; textRole:"label"; valueRole:"id"; Layout.fillWidth: true }
                                    Label { text: "Registr zmeškaných" }
                                    ComboBox { id: simMissed; model: [{label:"Bez změny",id:""},{label:"Zapnout",id:"true"},{label:"Vypnout",id:"false"}]; textRole:"label"; valueRole:"id"; Layout.fillWidth: true }
                                    Label { text: "LTE nyní" }
                                    ComboBox { id: simLte; model: [{label:"Bez změny",id:""},{label:"Zapnout",id:"true"},{label:"Vypnout",id:"false"}]; textRole:"label"; valueRole:"id"; Layout.fillWidth: true }
                                    Label { text: "LTE příští měsíc" }
                                    ComboBox { id: simLteNext; model: [{label:"Bez změny",id:""},{label:"Zapnout",id:"true"},{label:"Vypnout",id:"false"}]; textRole:"label"; valueRole:"id"; Layout.fillWidth: true }
                                    Label { text: "Roaming profil" }
                                    TextField { id: simRoaming; placeholderText: "ID profilu"; Layout.fillWidth: true }
                                    Label { text: "Premium služby" }
                                    ComboBox { id: simPremium; model: [{label:"Bez změny",id:""},{label:"Vypnout",id:"off"},{label:"SMS platby + DMS",id:"sms_payments_and_dms"},{label:"Vše kromě SMS plateb",id:"all_other_than_sms_payments_and_dms"},{label:"Vše",id:"all"}]; textRole:"label"; valueRole:"id"; Layout.fillWidth: true }
                                }
                                Label { visible: simState.currentValue; text: "Při změně stavu Odorik ignoruje ostatní parametry; THsip odešle jen stav."; color: "#b26a00" }
                                RowLayout {
                                    Button { text: "Zkontrolovat a změnit"; onClicked: { simPage.pendingChanges = simPage.changes(); confirmSimUpdate.open() } }
                                    Button { text: "Restart/FUP dat"; onClicked: confirmSimRestart.open() }
                                    Button { text: "Přiřadit fyzickou SIM"; onClicked: assignSimDialog.open() }
                                }
                                Label { text: "Historie mobilních dat"; font.pixelSize: 18; font.bold: true }
                                ListView {
                                    Layout.fillWidth: true; Layout.preferredHeight: Math.min(200, contentHeight); model: backend.simData; clip: true
                                    delegate: RowLayout {
                                        required property var modelData; width: ListView.view.width
                                        Label { text: modelData.date || modelData.time || "—"; Layout.preferredWidth: 170 }
                                        Label { text: "↓ " + (modelData.download || modelData.bytes_down || 0) + " · ↑ " + (modelData.upload || modelData.bytes_up || 0); Layout.fillWidth: true }
                                        Label { text: modelData.price !== undefined ? root.money(modelData.price) : "" }
                                    }
                                }
                            }
                        }
                        ApiPanel { panelTitle: "Kompletní SIM REST API"; category: "SIM" }
                    }
                }
            }

            Pane {
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "Kontakty"; font.pixelSize: 28; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button { text: "Import macOS"; onClicked: backend.importSystemContacts() }
                        Button { text: "Import CSV/vCard"; onClicked: contactFile.open() }
                        Button { text: "Obnovit"; onClicked: backend.refreshContacts() }
                    }
                    TextField { id: contactSearch; placeholderText: "Hledat jméno, firmu nebo číslo"; Layout.fillWidth: true }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4
                        model: backend.contacts
                        delegate: Frame {
                            id: contactRow
                            required property var modelData
                            property var contactData: modelData
                            width: ListView.view.width
                            visible: !contactSearch.text || (contactData.name + " " + contactData.organization + " " + contactData.numbers.map(n => n.number).join(" ")).toLowerCase().includes(contactSearch.text.toLowerCase())
                            height: visible ? implicitHeight : 0
                            ColumnLayout {
                                anchors.fill: parent
                                Label { text: contactRow.contactData.name; font.bold: true }
                                Label { text: contactRow.contactData.organization; visible: !!text; opacity: .7 }
                                Repeater {
                                    model: contactRow.contactData.numbers
                                    RowLayout {
                                        required property var modelData
                                        Label { text: (modelData.label ? modelData.label + ": " : "") + modelData.number; Layout.preferredWidth: 300 }
                                        Label { text: modelData.normalized === modelData.number ? "" : modelData.normalized; opacity: .6; Layout.fillWidth: true }
                                        Button { text: "Volat"; onClicked: { destination.text = modelData.normalized; pages.currentIndex = 1 } }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Pane {
                id: routingPage
                property var draftTargets: []
                property var preview: backend.routingPreview(routingNumber.currentValue || "", routingMode.currentValue || "parallel", draftTargets, {
                    timePeriod: timePeriod.text, timeFallback: timeFallback.text,
                    concurrencyEnabled: concurrencyEnabled.checked, concurrencyLine: concurrencyLine.text,
                    concurrencyMax: concurrencyMax.value, concurrencyFallback: concurrencyFallback.text
                })
                function addDraft() {
                    draftTargets = draftTargets.concat([{
                        kind: targetKind.currentValue, target: routingTarget.text, delay: targetDelay.value,
                        mailboxDelay: mailboxDelay.text, greeting: greetingId.text, queue: queueName.text,
                        slot: webIvrSlot.text, main: mainBranch.checked, forwardMode: forwardMode.currentValue,
                        preconnectGreeting: preconnectGreeting.text
                    }])
                }
                function applyRingings() { if (preview.valid) for (const target of preview.targets) backend.addRinging(routingNumber.currentValue, target) }
                function applyRoutes() { if (preview.valid) for (const target of preview.targets) backend.addRoute(routingNumber.currentValue, routeSource.text || "*", target, replaceSource.checked) }
                Component.onCompleted: backend.refreshRouting()

                Dialog {
                    id: deleteRouting
                    property string kind: ""
                    property string value: ""
                    title: "Potvrdit smazání routingu"
                    modal: true; standardButtons: Dialog.Ok | Dialog.Cancel
                    Label { text: deleteRouting.value; wrapMode: Text.WrapAnywhere }
                    onAccepted: kind === "route" ? backend.deleteRoute(routingNumber.currentValue, value, true) : backend.deleteRinging(routingNumber.currentValue, value, true)
                }

                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "Routing studio"; font.pixelSize: 28; font.bold: true }
                        ComboBox {
                            id: routingNumber; Layout.fillWidth: true; model: backend.routingNumbers
                            textRole: "public_number"; valueRole: "public_number"
                            onCountChanged: if (count && currentIndex >= 0) backend.loadRouting(currentValue)
                            onActivated: backend.loadRouting(currentValue)
                        }
                        Button { text: "Obnovit"; onClicked: backend.refreshRouting() }
                    }

                    RowLayout {
                        Layout.fillWidth: true; Layout.preferredHeight: 150
                        Frame {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            ColumnLayout {
                                anchors.fill: parent
                                Label { text: "Paralelní zvonění"; font.bold: true }
                                ListView {
                                    Layout.fillWidth: true; Layout.fillHeight: true; model: backend.routingRingings; clip: true
                                    delegate: RowLayout {
                                        required property var modelData; width: ListView.view.width
                                        Label { text: modelData.ringing_number; Layout.fillWidth: true; elide: Text.ElideMiddle }
                                        Button { text: "Smazat"; onClicked: { deleteRouting.kind = "ringing"; deleteRouting.value = modelData.ringing_number; deleteRouting.open() } }
                                    }
                                }
                            }
                        }
                        Frame {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            ColumnLayout {
                                anchors.fill: parent
                                Label { text: "Caller-specific routes"; font.bold: true }
                                ListView {
                                    Layout.fillWidth: true; Layout.fillHeight: true; model: backend.routingRoutes; clip: true
                                    delegate: RowLayout {
                                        required property var modelData; width: ListView.view.width
                                        Label { text: modelData.source_number + "  →  " + modelData.ringing_number; Layout.fillWidth: true; elide: Text.ElideMiddle }
                                        Button { text: "Smazat"; onClicked: { deleteRouting.kind = "route"; deleteRouting.value = String(modelData.id); deleteRouting.open() } }
                                    }
                                }
                            }
                        }
                    }

                    Label { text: "Nový cíl"; font.pixelSize: 20; font.bold: true }
                    RowLayout {
                        ComboBox {
                            id: targetKind; Layout.fillWidth: true; textRole: "label"; valueRole: "id"
                            model: [{label:"Běžný cíl",id:"normal"},{label:"Voicemail",id:"voicemail"},{label:"Pouze hláška",id:"greeting"},{label:"Voicemail jen mobil",id:"mobileVoicemail"},{label:"Hláška a vytočit",id:"greetingThenDial"},{label:"Odorik queue",id:"queue"},{label:"Web IVR",id:"webIvr"},{label:"Falešné zvonění",id:"falseRinging"},{label:"Obsazeno",id:"busy"}]
                        }
                        TextField { id: routingTarget; placeholderText: "Číslo, linka nebo SIP URI"; Layout.fillWidth: true; visible: ["normal","greetingThenDial"].includes(targetKind.currentValue) }
                        Label { text: "Zpoždění" }
                        SpinBox { id: targetDelay; from: 0; to: 99; editable: true }
                        CheckBox { id: mainBranch; text: "Hlavní větev"; enabled: targetKind.currentValue === "normal" }
                    }
                    RowLayout {
                        TextField { id: mailboxDelay; text: "20"; placeholderText: "Prodleva schránky"; visible: ["voicemail","greeting","mobileVoicemail","greetingThenDial"].includes(targetKind.currentValue) }
                        TextField { id: greetingId; text: "1"; placeholderText: "ID hlášky"; visible: ["voicemail","greeting","mobileVoicemail","greetingThenDial"].includes(targetKind.currentValue) }
                        TextField { id: queueName; placeholderText: "Název aktivované queue"; visible: targetKind.currentValue === "queue"; Layout.fillWidth: true }
                        TextField { id: webIvrSlot; text: "1"; placeholderText: "Web IVR slot"; visible: targetKind.currentValue === "webIvr" }
                        ComboBox { id: forwardMode; model: [{label:"Původní CLIP",id:""},{label:"CLIP linky",id:"replace"},{label:"CLIP linky + SMS",id:"sms"}]; textRole:"label"; valueRole:"id"; Layout.fillWidth: true }
                        TextField { id: preconnectGreeting; placeholderText: "Hláška volanému (volitelné)"; Layout.fillWidth: true }
                        Button { text: "Přidat do návrhu"; onClicked: routingPage.addDraft() }
                    }

                    RowLayout {
                        ComboBox { id: routingMode; model: [{label:"Paralelně",id:"parallel"},{label:"Náhodný cíl",id:"random"}]; textRole:"label"; valueRole:"id" }
                        TextField { id: timePeriod; placeholderText: "ID časového období (volitelné)"; Layout.fillWidth: true }
                        TextField { id: timeFallback; placeholderText: "Cíl mimo období"; Layout.fillWidth: true }
                        CheckBox { id: concurrencyEnabled; text: "Limit souběhu" }
                    }
                    RowLayout {
                        visible: concurrencyEnabled.checked
                        TextField { id: concurrencyLine; placeholderText: "Linka"; Layout.fillWidth: true }
                        SpinBox { id: concurrencyMax; from: 1; to: 99; value: 1 }
                        TextField { id: concurrencyFallback; text: "*0821"; placeholderText: "Fallback"; Layout.fillWidth: true }
                    }
                    ListView {
                        Layout.fillWidth: true; Layout.preferredHeight: Math.min(120, contentHeight); model: routingPage.draftTargets; clip: true
                        delegate: RowLayout {
                            required property var modelData; required property int index; width: ListView.view.width
                            Label { text: modelData.kind + (modelData.target ? ": " + modelData.target : ""); Layout.fillWidth: true }
                            Button { text: "Odebrat"; onClicked: routingPage.draftTargets = routingPage.draftTargets.filter((_, i) => i !== index) }
                        }
                    }
                    TextArea { text: routingPage.preview.preview || ""; readOnly: true; Layout.fillWidth: true; Layout.preferredHeight: 70; font.family: "Menlo" }
                    Label { text: (routingPage.preview.errors || []).join(" · "); visible: !!text; color: "#b00020"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    RowLayout {
                        TextField { id: routeSource; text: "*"; placeholderText: "Caller nebo *"; Layout.fillWidth: true }
                        CheckBox { id: replaceSource; text: "Nahradit existující source route" }
                        Button { text: "Přidat jako ringings"; enabled: routingPage.preview.valid; onClicked: routingPage.applyRingings() }
                        Button { text: "Přidat jako caller routes"; enabled: routingPage.preview.valid; onClicked: routingPage.applyRoutes() }
                        Button { text: "Vyčistit návrh"; onClicked: routingPage.draftTargets = [] }
                    }
                }
            }

            Pane {
                id: recordingsPage
                Component.onCompleted: backend.refreshRecordings()
                Dialog {
                    id: deleteRecordingDialog
                    property string recordingId: ""
                    title: "Trvale smazat serverovou nahrávku?"
                    modal: true; standardButtons: Dialog.Ok | Dialog.Cancel
                    Label { text: "Lokální soubor se přesune do Koše. Bridge objekt bude smazán ze storage."; wrapMode: Text.WordWrap }
                    onAccepted: backend.deleteRecording(recordingId, true)
                }
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "Nahrávky a voicemail"; font.pixelSize: 28; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button { text: recordingPlayer.playbackState === MediaPlayer.PlayingState ? "Pauza" : "Přehrát"; enabled: !!recordingPlayer.source; onClicked: recordingPlayer.playbackState === MediaPlayer.PlayingState ? recordingPlayer.pause() : recordingPlayer.play() }
                        Button { text: "Obnovit"; onClicked: backend.refreshRecordings() }
                    }
                    Label { text: "Lokální PJSUA2 recording + Bridge uploady párované přes Call-ID. Žádný neexistující Odorik bulk-download."; opacity: .7; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 6
                        model: backend.recordings
                        delegate: Frame {
                            id: recordingRow
                            required property var modelData
                            width: ListView.view.width
                            ColumnLayout {
                                anchors.fill: parent
                                RowLayout {
                                    Label { text: recordingRow.modelData.local ? "Lokální" : "Bridge"; font.bold: true }
                                    Label { text: "Call-ID: " + (recordingRow.modelData.callId || "—"); Layout.fillWidth: true; elide: Text.ElideMiddle }
                                    Label { text: recordingRow.modelData.mime + " · " + Number(recordingRow.modelData.size || 0).toLocaleString() + " B"; opacity: .7 }
                                    Button { text: "Přehrát"; onClicked: backend.playRecording(recordingRow.modelData.id) }
                                    Button { text: "Export"; onClicked: { root.exportRecordingId = recordingRow.modelData.id; recordingExport.open() } }
                                    Button { text: "Smazat"; onClicked: { deleteRecordingDialog.recordingId = recordingRow.modelData.id; deleteRecordingDialog.open() } }
                                }
                                RowLayout {
                                    TextField { id: recordingNotes; text: recordingRow.modelData.notes || ""; placeholderText: "Poznámka"; Layout.fillWidth: true }
                                    TextField { id: recordingRetention; text: recordingRow.modelData.retainUntil || ""; placeholderText: "Retence ISO, např. 2027-01-01T00:00:00Z"; Layout.fillWidth: true }
                                    Button { text: "Uložit metadata"; onClicked: backend.updateRecording(recordingRow.modelData.id, recordingNotes.text, recordingRetention.text) }
                                }
                                Label { text: recordingRow.modelData.path || (recordingRow.modelData.metadata ? JSON.stringify(recordingRow.modelData.metadata) : ""); opacity: .55; elide: Text.ElideMiddle; Layout.fillWidth: true }
                            }
                        }
                    }
                }
            }

            Pane {
                ColumnLayout {
                    id: functionPage
                    anchors.fill: parent
                    property var selectedAction: actionPicker.currentIndex >= 0 && actionPicker.currentIndex < backend.dialActions.length ? backend.dialActions[actionPicker.currentIndex] : ({parameters:[]})
                    function actionParameters() {
                        let result = {}
                        for (let i = 0; i < actionParameterRepeater.count; ++i) {
                            let row = actionParameterRepeater.itemAt(i)
                            result[row.parameter.name] = row.value
                        }
                        return result
                    }
                    Label { text: "Odorik funkce"; font.pixelSize: 28; font.bold: true }
                    RowLayout {
                        ComboBox { id: actionPicker; model: backend.dialActions; textRole: "title"; Layout.fillWidth: true }
                        Switch { id: advancedActions; text: "Advanced" }
                    }
                    Repeater {
                        id: actionParameterRepeater
                        model: functionPage.selectedAction.parameters || []
                        RowLayout {
                            required property var modelData
                            property var parameter: modelData
                            property alias value: actionValue.text
                            Layout.fillWidth: true
                            Label { text: modelData.label; Layout.preferredWidth: 240 }
                            TextField { id: actionValue; text: modelData.default || ""; Layout.fillWidth: true }
                        }
                    }
                    TextField {
                        id: actionPreview
                        Layout.fillWidth: true; readOnly: true
                        text: backend.compileDialAction(functionPage.selectedAction.id || "", functionPage.actionParameters(), advancedActions.checked)
                    }
                    RowLayout {
                        Button { text: "Použít v dialeru"; enabled: !actionPreview.text.startsWith("Chyba"); onClicked: { destination.text = actionPreview.text; pages.currentIndex = 1 } }
                        Label { text: functionPage.selectedAction.advanced ? "Advanced / legacy / support-enabled funkce vyžaduje vědomé zapnutí." : "Přímo dostupná Odorik funkce."; opacity: .7; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    }
                    Label { text: "Capability katalog"; font.pixelSize: 20; font.bold: true }
                    TextField { id: capabilitySearch; placeholderText: "Hledat funkci"; Layout.fillWidth: true }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: backend.capabilities
                        delegate: ItemDelegate {
                            required property var modelData
                            width: ListView.view.width
                            visible: !capabilitySearch.text || (modelData.id + " " + modelData.title + " " + modelData.category).toLowerCase().includes(capabilitySearch.text.toLowerCase())
                            height: visible ? implicitHeight : 0
                            contentItem: Column {
                                Label { text: modelData.title; font.bold: true }
                                Label { text: modelData.category + " · " + modelData.mechanism + " · " + modelData.status; opacity: .7 }
                            }
                        }
                    }
                }
            }

            Pane {
                id: bridgePage
                Component.onCompleted: backend.refreshBridgeEvents()
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "THsip Bridge"; font.pixelSize: 28; font.bold: true }
                        Label { text: backend.bridgeLive ? "WebSocket živý" : "Odpojen"; color: backend.bridgeLive ? "#2e7d32" : "#b26a00" }
                        Item { Layout.fillWidth: true }
                        Button { text: "Obnovit události"; onClicked: backend.refreshBridgeEvents() }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; Layout.alignment: Qt.AlignTop
                        Frame {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            ColumnLayout {
                                anchors.fill: parent
                                Label { text: "Remote IVR builder"; font.pixelSize: 20; font.bold: true }
                                RowLayout {
                                    Label { text: "Slot" }
                                    SpinBox { id: bridgeIvrSlot; from: 1; to: 99; value: 1; editable: true }
                                    Button { text: "Načíst"; onClicked: backend.loadBridgeIvr(bridgeIvrSlot.value) }
                                    Button { text: "Uložit"; onClicked: backend.saveBridgeIvr(bridgeIvrSlot.value, bridgeIvrScript.text) }
                                }
                                TextArea {
                                    id: bridgeIvrScript
                                    Layout.fillWidth: true; Layout.fillHeight: true; font.family: "Menlo"
                                    text: backend.ivrScript
                                    placeholderText: "answer\nplay:1\ndial:*08320+420123456789"
                                    wrapMode: TextEdit.NoWrap
                                }
                                Label { text: "Povolené: answer, play, play2, playnumber, dial, setclip, hangup, uri. Max 100 příkazů."; wrapMode: Text.WordWrap; Layout.fillWidth: true; opacity: .65 }
                            }
                        }
                        Frame {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            ColumnLayout {
                                anchors.fill: parent
                                Label { text: "Webhooky 98/99"; font.pixelSize: 20; font.bold: true }
                                ListView {
                                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4; model: backend.bridgeEvents
                                    delegate: Frame {
                                        id: bridgeEventRow
                                        required property var modelData
                                        width: ListView.view.width
                                        ColumnLayout {
                                            anchors.fill: parent
                                            RowLayout {
                                                Label { text: "Webhook " + bridgeEventRow.modelData.kind; font.bold: true }
                                                Label { text: bridgeEventRow.modelData.occurredAt || "—"; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; opacity: .65 }
                                            }
                                            Label { text: (bridgeEventRow.modelData.from || "—") + "  →  " + (bridgeEventRow.modelData.to || "—") + " · linka " + (bridgeEventRow.modelData.line || "—"); Layout.fillWidth: true; elide: Text.ElideMiddle }
                                            Label { text: "Call-ID " + (bridgeEventRow.modelData.callId || "—") + " · " + (bridgeEventRow.modelData.status || ""); Layout.fillWidth: true; elide: Text.ElideMiddle; opacity: .65 }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Pane {
                id: diagnosticsPage
                Component.onCompleted: {
                    root.systemDiagnosticData = backend.systemDiagnostics()
                    if (backend.pjsipAvailable) root.telephonyDiagnostics = telephony.diagnostics()
                }
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "Diagnostika"; font.pixelSize: 28; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button { text: "DNS/TCP/TLS test"; onClicked: backend.runNetworkDiagnostics() }
                        Button { text: "Obnovit PJSIP"; enabled: backend.pjsipAvailable; onClicked: root.telephonyDiagnostics = telephony.diagnostics() }
                        Button { text: "Export redacted bundle"; onClicked: backend.exportDiagnostics(backend.pjsipAvailable ? telephony.diagnostics() : {}) }
                    }
                    RowLayout {
                        TextField { id: diagnosticStun; text: "stun.odorik.cz:3478"; placeholderText: "STUN server"; Layout.fillWidth: true }
                        Button { text: "STUN/NAT test"; enabled: backend.pjsipAvailable; onClicked: telephony.detectNat(diagnosticStun.text) }
                        Button { text: "Test tón"; onClicked: testTone.play() }
                        CheckBox { text: "Mikrofon do reproduktoru"; enabled: backend.pjsipAvailable; onToggled: telephony.setAudioLoopback(checked) }
                        CheckBox { text: "Kamera"; onToggled: diagnosticCamera.active = checked }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.preferredHeight: 180
                        Frame {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            ColumnLayout {
                                anchors.fill: parent
                                Label { text: "Systém / PJSIP"; font.bold: true }
                                TextArea { Layout.fillWidth: true; Layout.fillHeight: true; readOnly: true; font.family: "Menlo"; wrapMode: TextEdit.WrapAnywhere; text: JSON.stringify({system:root.systemDiagnosticData,telephony:root.telephonyDiagnostics}, null, 2) }
                            }
                        }
                        VideoOutput { id: diagnosticVideo; Layout.preferredWidth: 260; Layout.fillHeight: true; visible: diagnosticCamera.active }
                    }
                    Label { text: "Odorik síťové testy"; font.pixelSize: 20; font.bold: true }
                    ListView {
                        Layout.fillWidth: true; Layout.preferredHeight: Math.min(160, contentHeight); model: backend.networkDiagnostics; clip: true
                        delegate: RowLayout {
                            required property var modelData; width: ListView.view.width
                            Label { text: modelData.kind.toUpperCase(); Layout.preferredWidth: 55; font.bold: true }
                            Label { text: modelData.target; Layout.preferredWidth: 190 }
                            Label { text: modelData.status; color: modelData.status === "ok" ? "#2e7d32" : modelData.status === "running" ? palette.text : "#b00020"; Layout.preferredWidth: 70 }
                            Label { text: modelData.detail || ""; Layout.fillWidth: true; elide: Text.ElideMiddle }
                        }
                    }
                    RowLayout {
                        Label { text: "Odorik audio testy:"; font.bold: true }
                        Button { text: "DTMF *080"; onClicked: { destination.text = "*080"; pages.currentIndex = 1 } }
                        Button { text: "Echo *081"; onClicked: { destination.text = "*081"; pages.currentIndex = 1 } }
                        Button { text: "Recording *0811"; onClicked: { destination.text = "*0811"; pages.currentIndex = 1 } }
                        Label { text: "ALG symptom: selhává 5060, ale 6688/443 funguje. Aplikace pouze ukáže výsledky; nedeklaruje jistotu bez SIP trace."; Layout.fillWidth: true; wrapMode: Text.WordWrap; opacity: .7 }
                    }
                    Label { text: "Registration / NAT / call log"; font.pixelSize: 20; font.bold: true }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; model: root.diagnosticEvents; clip: true
                        delegate: Label { required property var modelData; width: ListView.view.width; text: modelData.at + " · " + modelData.kind + " · " + JSON.stringify(modelData); elide: Text.ElideRight }
                    }
                }
            }

            Pane {
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Label { text: "Oficiální portál"; font.pixelSize: 28; font.bold: true }
                        Item { Layout.fillWidth: true }
                        ComboBox { id: portalPage; model: ["home", "lines", "audio", "fax", "limits", "help"]; onActivated: web.url = backend.portalUrl(currentText) }
                        Button { text: "Externě"; onClicked: backend.openExternal(web.url) }
                    }
                    WebEngineView {
                        id: web
                        Layout.fillWidth: true; Layout.fillHeight: true
                        url: backend.portalUrl("home")
                        onNavigationRequested: function(request) {
                            if (!backend.isOfficialPortalUrl(request.url)) {
                                request.reject()
                                backend.openExternal(request.url)
                            }
                        }
                    }
                }
            }

            Pane {
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: "Odorik API účet"; font.pixelSize: 28; font.bold: true }
                    TextField { id: user; placeholderText: "ID účtu nebo číslo linky"; Layout.fillWidth: true }
                    TextField { id: password; placeholderText: "API/SIP heslo"; echoMode: TextInput.Password; Layout.fillWidth: true }
                    Button { text: "Uložit bezpečně"; onClicked: { backend.configureAccount(user.text, password.text); password.clear() } }
                    Label { text: "SIP linka"; font.pixelSize: 20; font.bold: true }
                    TextField { id: sipLine; placeholderText: "Šestimístná Odorik linka"; Layout.fillWidth: true }
                    TextField { id: sipPassword; placeholderText: "SIP heslo"; echoMode: TextInput.Password; Layout.fillWidth: true }
                    ComboBox { id: callerNameMode; model: ["Běžný", "Anonymous", "Nahravany_hovor", "Nahravany_Anonym"]; Layout.fillWidth: true }
                    RowLayout {
                        TextField { id: stunServer; placeholderText: "STUN server (volitelné)"; Layout.fillWidth: true }
                        TextField { id: turnServer; placeholderText: "TURN host:port (volitelné)"; Layout.fillWidth: true }
                    }
                    RowLayout {
                        TextField { id: turnUser; placeholderText: "TURN uživatel"; Layout.fillWidth: true }
                        TextField { id: turnPassword; placeholderText: "TURN heslo"; echoMode: TextInput.Password; Layout.fillWidth: true }
                    }
                    Button {
                        text: "Registrovat přes TLS/SRTP"
                        enabled: backend.pjsipAvailable
                        onClicked: {
                            backend.storeSipPassword(sipLine.text, sipPassword.text)
                            root.sipAccountId = telephony.addAccount({
                                idUri: (callerNameMode.currentIndex ? "\"" + callerNameMode.currentText + "\" " : "") + "<sip:" + sipLine.text + "@sip.odorik.cz>",
                                registrar: "sips:sip.odorik.cz:5061;transport=tls",
                                user: sipLine.text,
                                password: sipPassword.text,
                                srtp: true,
                                ice: true,
                                stunServer: stunServer.text,
                                turnServer: turnServer.text,
                                turnUser: turnUser.text,
                                turnPassword: turnPassword.text
                            })
                            sipPassword.clear()
                            turnPassword.clear()
                        }
                    }
                    Label { text: "Média a kodeky"; font.pixelSize: 20; font.bold: true }
                    Button {
                        text: "Načíst zařízení a kodeky"
                        enabled: backend.pjsipAvailable
                        onClicked: {
                            root.audioDeviceList = telephony.audioDevices()
                            root.videoDeviceList = telephony.videoDevices()
                            root.codecList = telephony.codecs()
                        }
                    }
                    RowLayout {
                        Label { text: "Mikrofon" }
                        ComboBox { id: captureDevice; Layout.fillWidth: true; model: root.audioDeviceList.filter(device => device.inputs > 0); textRole: "name"; valueRole: "id" }
                        Label { text: "Výstup" }
                        ComboBox { id: playbackDevice; Layout.fillWidth: true; model: root.audioDeviceList.filter(device => device.outputs > 0); textRole: "name"; valueRole: "id" }
                        Button { text: "Použít"; enabled: captureDevice.currentIndex >= 0 && playbackDevice.currentIndex >= 0; onClicked: telephony.setAudioDevices(captureDevice.currentValue, playbackDevice.currentValue) }
                    }
                    Label { text: "Kamery: " + root.videoDeviceList.map(device => device.name).join(", "); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: "Kodeky: " + root.codecList.filter(codec => codec.priority > 0).map(codec => codec.id + " (" + codec.priority + ")").join(", "); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: "macOS: heslo v systémovém Keychainu. Windows/Linux build použije QtKeychain adaptér."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: "THsip Bridge"; font.pixelSize: 20; font.bold: true }
                    RowLayout {
                        TextField { id: bridgeUrl; placeholderText: "https://bridge.example.cz/"; Layout.fillWidth: true }
                        TextField { id: bridgeToken; placeholderText: "Client bearer token"; echoMode: TextInput.Password; Layout.fillWidth: true }
                        Button { text: "Uložit Bridge"; onClicked: { backend.configureBridge(bridgeUrl.text, bridgeToken.text); bridgeToken.clear() } }
                    }
                    Label { text: "Bezpečnost"; font.pixelSize: 20; font.bold: true }
                    Label { text: "API pouze HTTPS · credentials form-encoded · externí portálové odkazy v systémovém browseru · žádný DOM scraping · žádné privátní endpointy"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
