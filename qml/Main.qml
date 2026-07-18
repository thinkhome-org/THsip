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
        { label: "Funkce", page: 8 }, { label: "Portál", page: 9 },
        { label: "Nastavení", page: 10 }
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

        Component.onCompleted: choose(0)

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
                        TextField { id: smsSender; placeholderText: "Odesílatel (volitelné)"; Layout.fillWidth: true }
                        TextField { id: smsDelayed; placeholderText: "Odložit: minuty nebo čas"; Layout.fillWidth: true }
                    }
                    TextArea { id: smsMessage; placeholderText: "Text, maximálně 765 znaků"; Layout.fillWidth: true; Layout.preferredHeight: 110; wrapMode: TextEdit.Wrap }
                    property var info: backend.smsInfo(smsMessage.text)
                    RowLayout {
                        Label { text: smsComposer.info.encoding + " · " + smsComposer.info.units + " jednotek · " + smsComposer.info.segments + " segmentů · zbývá " + smsComposer.info.remaining; color: smsComposer.info.valid ? palette.text : "#b00020" }
                        Item { Layout.fillWidth: true }
                        Button { text: "Odeslat SMS"; enabled: smsComposer.info.valid && !!smsRecipient.text && !!smsMessage.text; onClicked: backend.api("POST", "sms", {recipient:smsRecipient.text, message:smsMessage.text, sender:smsSender.text, delayed:smsDelayed.text}) }
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
                    Label { text: "Přehled"; font.pixelSize: 28; font.bold: true }
                    RowLayout {
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Kredit"; opacity: .7 } Label { text: backend.balance; font.pixelSize: 28 } Button { text: "Obnovit"; onClicked: backend.refreshBalance() } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Capability katalog"; opacity: .7 } Label { text: backend.capabilities.length + " položek"; font.pixelSize: 28 } Label { text: "REST, SIP, prefixy, bridge, portál" } } }
                        Frame { Layout.fillWidth: true; ColumnLayout { Label { text: "Lokální data"; opacity: .7 } Label { text: "SQLite"; font.pixelSize: 28 } Label { text: backend.databasePath(); elide: Text.ElideMiddle; Layout.maximumWidth: 270 } } }
                    }
                    Label { text: "Rychlé akce"; font.pixelSize: 20; font.bold: true }
                    RowLayout {
                        Button { text: "Zavolat"; onClicked: pages.currentIndex = 1 }
                        Button { text: "SMS"; onClicked: pages.currentIndex = 3 }
                        Button { text: "Callback"; onClicked: pages.currentIndex = 2 }
                        Button { text: "Routing"; onClicked: pages.currentIndex = 6 }
                    }
                    TextArea { Layout.fillWidth: true; Layout.preferredHeight: 300; readOnly: true; text: backend.output; wrapMode: TextEdit.WrapAnywhere }
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

            ApiPanel { panelTitle: "Kompletní Odorik REST API" }
            ApiPanel { panelTitle: "SMS"; category: "SMS" }
            ApiPanel { panelTitle: "Odorik SIM"; category: "SIM" }

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
