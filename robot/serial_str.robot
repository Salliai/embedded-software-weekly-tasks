*** Settings ***
Library   String
Library   SerialLibrary

*** Variables ***
${com}   	COM3
${baud} 	115200
${board}	nRF

${seq_GYG}	GYG
${seq_invalid_char}	RYaG
${seq_success}	000011
${seq_timer_char_error}	oaoaoa
${seq_timer_char_error2}	00001s
${seq_negative_error}	-00001
${seq_invalid_time_error}	909099

${time_len_error}    Time invalid: -1
${time_array_error}    Time invalid: -2
${time_value_error}    Time invalid: -3
${seq_char_error}	Invalid sequence!

${success} 	Timer for red led set for 11 seconds
${RYGRYG_success}	VALID SEQUENCE

*** Test Cases ***
Connect Serial
	Log To Console  Connecting to ${board}
	Add Port  ${com}  baudrate=${baud}  encoding=ascii
	Port Should Be Open  ${com}
	Reset Input Buffer
	Reset Output Buffer

Serial Timer Success
	# Tätä D kirjoitusta käytetään vain jos ei ole vielä kertaalleen isketty debuggia päälle
    #Write Data    D\r    encoding=ascii 
    Sleep    1000ms
    ${junk}=    Read All Data

    Write Data    ${seq_success}\r	encoding=ascii
    Sleep    1000ms

    ${raw}=    Read All Data
    ${read}=   Evaluate    bytes.fromhex("""${raw}""").decode("ascii", errors="ignore")

    #Log To Console    ${read}

    Should Contain    ${read}    Timer for red led set for 11 seconds

Serial Timer Character Error
	Sleep    1000ms
    ${junk}=    Read All Data

    Write Data    ${seq_timer_char_error}\r    encoding=ascii
    Sleep    1000ms

    ${raw}=    Read All Data
    ${read}=   Evaluate    bytes.fromhex("""${raw}""").decode("ascii", errors="ignore")

    #Log To Console    ${read}

    Should Contain    ${read}    ${time_value_error}
	
Serial Timer Negative Error
    Sleep    1000ms

    ${junk}=    Read All Data

    Log To Console    Sending '${seq_negative_error}'

    Write Data    ${seq_negative_error}\r	encoding=ascii

    Sleep    1000ms

    ${raw}=    Read All Data
    #Log To Console    RAW='${raw}'

    ${read}=    Evaluate    bytes.fromhex("""${raw}""").decode("ascii", errors="ignore")

    #Log To Console    READ='${read}'

    Should Contain    ${read}    ${time_value_error}

Serial Timer Numbers and Characters Error
    Sleep    1000ms

    ${junk}=    Read All Data

    Log To Console    Sending '${seq_timer_char_error2}'

    Write Data    ${seq_timer_char_error2}\r	encoding=ascii

    Sleep    1000ms

    ${raw}=    Read All Data
    #Log To Console    RAW='${raw}'

    ${read}=    Evaluate    bytes.fromhex("""${raw}""").decode("ascii", errors="ignore")

    #Log To Console    READ='${read}'

    Should Contain    ${read}    ${time_value_error}

Serial Timer Invalid Time Error
    Sleep    1000ms

    ${junk}=    Read All Data

    Log To Console    Sending '${seq_invalid_time_error}'

    Write Data    ${seq_invalid_time_error}\r	encoding=ascii

    Sleep    1000ms

    ${raw}=    Read All Data
    #Log To Console    RAW='${raw}'

    ${read}=    Evaluate    bytes.fromhex("""${raw}""").decode("ascii", errors="ignore")

    #Log To Console    READ='${read}'

    Should Contain    ${read}    ${time_len_error}	

Serial GYG sequence test
    Sleep    1000ms

    ${junk}=    Read All Data

    Log To Console    Sending '${seq_GYG}'

    Write Data    ${seq_GYG}\r	encoding=ascii

    Sleep	6000ms

    ${raw}=    Read All Data
    #Log To Console    RAW='${raw}'

    ${read}=    Evaluate    bytes.fromhex("""${raw}""").decode("ascii", errors="ignore")

    #Log To Console    READ='${read}'

    Should Contain    ${read}    ${RYGRYG_success}

Serial Seq Wrong Char Error Test
    Sleep    1000ms

    ${junk}=    Read All Data

    Log To Console    Sending '${seq_invalid_char}'

    Write Data    ${seq_invalid_char}\r	encoding=ascii
    
	Sleep	1000ms
	
	${raw}=    Read All Data
    #Log To Console    RAW='${raw}'

    ${read}=    Evaluate    bytes.fromhex("""${raw}""").decode("ascii", errors="ignore")

    #Log To Console    READ='${read}'

    Should Contain    ${read}    ${seq_char_error}	
	
Disconnect Serial
	Log To Console  Disconnecting ${board}
	[TearDown]  Delete Port  ${com}


	
	
	

