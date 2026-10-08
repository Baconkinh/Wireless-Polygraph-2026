// =====================================================================
//  web_page.h — หน้าเว็บที่ฝังอยู่ในนาฬิกา   ต่อ WiFi "Polygraph-Watch" แล้วเปิด http://192.168.4.1
//
//  หน้านี้ "อยู่ในเฟิร์มแวร์" (เก็บในแฟลชเป็น string ตัวเดียว) นาฬิกาจึงเป็นทั้ง
//    backend  = REST API ใน web_server.cpp  (/api/live, /api/lie/..., /api/ml/...)
//    frontend = หน้านี้ (HTML + CSS + JavaScript) ที่มือถือดาวน์โหลดไปรัน
//  ใช้ได้จากมือถือเครื่องเดียว ไม่ต้องมีโน้ตบุ๊ก
//  ส่วน Polygraph Studio บนโน้ตบุ๊กเป็นอีกโปรแกรมหนึ่งที่เรียก API ชุดเดียวกันนี้
//
//  v2.1
//   - ไอคอนเป็น SVG ที่วาดเอง ไม่ใช้ emoji (emoji หน้าตาไม่เหมือนกันในแต่ละเครื่อง และดูไม่ทางการ)
//   - โหมดเก็บข้อมูล (Train) / ใช้งานจริง (Detect), ดาวน์โหลด CSV, อัปโหลดโมเดล AI
//   - ตั้งค่าการหลับ / ECO / กำลังส่ง WiFi
//  v2.2 (ให้ฟีเจอร์ตามทัน Polygraph Studio บนคอม)
//   - ชุดคำถามสำเร็จรูป + "ถามข้อถัดไป" (วัดค่าปกติครั้งเดียวต่อคน), เกมทายเลขลับ, เพิ่ม/ลบคำถามเอง
//   - โหมดใช้งานจริง: กดบอก ถูก/ผิด/ไม่ทราบ หลังได้ผล -> บันทึกเป็นข้อมูลเทรน (POST /api/ml/feedback)
//     + สถิติความแม่นยำตอนใช้งานจริง
//   - อธิบายผล: สัญญาณไหนเปลี่ยนมากแค่ไหน (z ของ 5 สัญญาณ), รายการตรวจความพร้อมก่อนถาม
//   - ดู/ลบข้อมูลเทรนในนาฬิการายแถว (POST /api/ml/data/delete), ไฟล์ดาวน์โหลดมีวันเวลาในชื่อ
//   - ชุดคำถาม/ถูก-ผิด/ข้อความคำถาม จำไว้ในมือถือ (localStorage) ไม่กินหน่วยความจำนาฬิกา
//  ไม่โหลดอะไรจากอินเทอร์เน็ต (WiFi ของนาฬิกาไม่มีเน็ต) กราฟวาดด้วย <canvas> เอง
// =====================================================================
#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="th"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wireless Polygraph</title>
<style>
:root{--bg:#eef1f6;--card:#fff;--line:#d9dfe9;--tx:#18212f;--mut:#5f6b80;--pri:#173f73;--pril:#e8eef8;
--lie:#b42318;--liel:#fdecea;--tru:#1a7f37;--trul:#e7f5ec;--inc:#9a6700;--incl:#fff4d6;--na:#6b7280}
*{box-sizing:border-box}html{-webkit-text-size-adjust:100%}
body{margin:0;font:15px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans Thai",Tahoma,sans-serif;background:var(--bg);color:var(--tx)}
.ico{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;flex:none}
header{position:sticky;top:0;z-index:5;background:var(--pri);color:#fff;display:flex;align-items:center;gap:10px;padding:10px 14px}
header .ico.lg{width:28px;height:28px}header h1{margin:0;font-size:16px;font-weight:600}
header small{display:block;opacity:.85;font-size:12px}
.hs{margin-left:auto;display:flex;gap:12px;font-size:13px;align-items:center}.hs span{display:flex;gap:4px;align-items:center}
.dot{width:9px;height:9px;border-radius:50%;background:#9ca3af}.dot.on{background:#4ade80}
nav{position:sticky;top:52px;z-index:4;display:flex;gap:6px;overflow-x:auto;padding:8px 12px;background:var(--bg);border-bottom:1px solid var(--line)}
nav a{flex:none;font-size:13px;padding:5px 11px;border-radius:14px;background:#fff;border:1px solid var(--line);color:var(--pri);text-decoration:none}
main{max-width:820px;margin:0 auto;padding:12px;display:grid;grid-template-columns:minmax(0,1fr);gap:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;scroll-margin-top:100px;min-width:0}
h2{display:flex;align-items:center;gap:8px;margin:0 0 10px;font-size:15px;font-weight:600;color:var(--pri)}
h2 .r{margin-left:auto;font-size:12px;font-weight:400;color:var(--mut)}
.seg{display:grid;grid-template-columns:1fr 1fr;border:1px solid var(--pri);border-radius:8px;overflow:hidden}
.seg button{border:0;border-radius:0;background:#fff;color:var(--pri);font-weight:600;min-height:46px}
.seg button.on{background:var(--pri);color:#fff}
.hint{margin:8px 0 0;color:var(--mut);font-size:13px}
ol.steps{margin:8px 0 0;padding-left:20px;color:var(--mut);font-size:13px}ol.steps li{margin:2px 0}
button,.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;font:inherit;font-size:14px;padding:9px 12px;
border-radius:8px;border:1px solid var(--line);background:#f7f9fc;color:var(--tx);cursor:pointer;text-decoration:none;min-height:44px}
button:disabled{opacity:.4;cursor:not-allowed}
button.pri{background:var(--pri);border-color:var(--pri);color:#fff}
button.tru{background:var(--trul);border-color:var(--tru);color:var(--tru);font-weight:600}
button.lie{background:var(--liel);border-color:var(--lie);color:var(--lie);font-weight:600}
button.dng{color:var(--lie)}
button.sm{min-height:32px;padding:4px 10px;font-size:13px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}.row.one{grid-template-columns:1fr}.row.three{grid-template-columns:1fr 1fr 1fr}
.lbl{text-align:center;color:var(--mut);font-size:13px}
.vd{font-size:36px;font-weight:700;text-align:center;margin:2px 0 6px;letter-spacing:.5px}
.vd.lie{color:var(--lie)}.vd.truth{color:var(--tru)}.vd.inconclusive{color:var(--inc)}.vd.invalid{color:var(--na)}
.bar{height:8px;background:#e5e9f0;border-radius:4px;overflow:hidden}.bar i{display:block;height:100%;width:0;background:var(--pri);transition:width .4s}
.meta{display:flex;flex-wrap:wrap;gap:6px;justify-content:center;margin-top:10px}
.tag{font-size:12px;padding:2px 9px;border-radius:12px;background:var(--pril);color:var(--pri);white-space:nowrap}
.tag.w{background:var(--incl);color:var(--inc)}.tag.t{background:var(--trul);color:var(--tru)}.tag.l{background:var(--liel);color:var(--lie)}
.state{display:flex;align-items:center;gap:8px;padding:10px;border-radius:8px;background:var(--pril);color:var(--pri);font-weight:600}
.chk{display:flex;gap:8px;align-items:flex-start;font-size:13px;margin-top:6px}
.chk b{width:20px;height:20px;border-radius:50%;display:inline-flex;align-items:center;justify-content:center;font-size:12px;flex:none;color:#fff;background:var(--na)}
.chk b.ok{background:var(--tru)}.chk b.wn{background:var(--inc)}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.tile{border:1px solid var(--line);border-radius:8px;padding:8px 10px}
.tile .h{display:flex;align-items:center;gap:6px;color:var(--mut);font-size:12px}
.tile b{display:block;font-size:21px;font-weight:600;margin-top:2px}.tile b small{font-size:12px;font-weight:400;color:var(--mut)}
.tile em{display:block;font-style:normal;font-size:12px;color:var(--mut);min-height:18px}
.tile.off b{color:var(--na);font-size:15px;font-weight:500}
.tabs{display:flex;gap:6px;margin:12px 0 6px;flex-wrap:wrap}
.tabs button{min-height:32px;padding:4px 12px;font-size:13px}.tabs button.on{background:var(--pri);color:#fff;border-color:var(--pri)}
canvas{width:100%;height:150px;display:block;border:1px solid var(--line);border-radius:8px}
.tw{overflow-x:auto}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{padding:7px 4px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}th{color:var(--mut);font-weight:500}
td.lie{color:var(--lie);font-weight:600}td.truth{color:var(--tru);font-weight:600}td.inconclusive{color:var(--inc)}td.invalid{color:var(--na)}
.kv{display:flex;justify-content:space-between;gap:10px;padding:7px 0;border-bottom:1px solid var(--line);font-size:14px}
.kv span:first-child{color:var(--mut);display:flex;align-items:center;gap:6px}
select,input[type=text]{font:inherit;padding:8px;border:1px solid var(--line);border-radius:8px;background:#fff;color:var(--tx);min-height:42px;min-width:0}
.subj{display:flex;gap:8px;margin-top:10px}.subj input,.subj select{flex:1;min-width:0}
.cnt{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:10px;text-align:center}
.cnt.four{grid-template-columns:repeat(4,1fr)}
.cnt div{border:1px solid var(--line);border-radius:8px;padding:6px}.cnt b{display:block;font-size:20px}.cnt span{font-size:12px;color:var(--mut)}
.msg{min-height:22px;margin-top:10px;font-size:14px}.msg.ok{color:var(--tru)}.msg.bad{color:var(--lie)}
.q{display:grid;grid-template-columns:28px 1fr auto;gap:8px;align-items:center;padding:8px;border:1px solid var(--line);border-radius:8px;margin-top:6px}
.q.cur{border-color:var(--pri);background:var(--pril)}.q.done{background:#fafbfd}
.q .n{width:26px;height:26px;border-radius:50%;background:var(--pril);color:var(--pri);display:flex;align-items:center;justify-content:center;font-size:13px;font-weight:600}
.q .tx{font-size:14px}.q .sub{display:flex;flex-wrap:wrap;gap:4px;margin-top:3px}
.q .ac{display:flex;gap:4px}
.sig{display:grid;grid-template-columns:110px 1fr 70px;gap:8px;align-items:center;font-size:13px;margin-top:5px}
.sig .tr{height:8px;background:#e5e9f0;border-radius:4px;overflow:hidden}.sig .tr i{display:block;height:100%}
.fb{margin-top:12px;padding:10px;border-radius:8px;background:#f7f9fc;border:1px dashed var(--line)}
footer{text-align:center;color:var(--mut);font-size:12px;padding:4px 0 20px}a{color:var(--pri)}
.hide{display:none!important}
@media(max-width:520px){.grid{grid-template-columns:repeat(2,1fr)}.vd{font-size:30px}.hs .t{display:none}.sig{grid-template-columns:90px 1fr 60px}}
</style></head><body>
<svg width="0" height="0" style="position:absolute" aria-hidden="true">
<symbol id="i-logo" viewBox="0 0 24 24"><rect x="6" y="2" width="12" height="20" rx="3"/><path d="M8 12h2l1-3 2 6 1-3h2"/></symbol>
<symbol id="i-heart" viewBox="0 0 24 24"><path d="M12 20s-7.5-4.6-9.2-9.4A4.8 4.8 0 0 1 12 7.4a4.8 4.8 0 0 1 9.2 3.2C19.5 15.4 12 20 12 20z"/></symbol>
<symbol id="i-pulse" viewBox="0 0 24 24"><path d="M2 12h4l2-5 4 10 2-5h8"/></symbol>
<symbol id="i-drop" viewBox="0 0 24 24"><path d="M12 3s6 6.4 6 11a6 6 0 0 1-12 0c0-4.6 6-11 6-11z"/></symbol>
<symbol id="i-temp" viewBox="0 0 24 24"><path d="M10 14.5V5a2 2 0 0 1 4 0v9.5a4 4 0 1 1-4 0z"/><path d="M12 17v-6"/></symbol>
<symbol id="i-wave" viewBox="0 0 24 24"><path d="M2 12c1.5-4 3-4 4.5 0s3 4 4.5 0 3-4 4.5 0 3 4 4.5 0"/></symbol>
<symbol id="i-gauge" viewBox="0 0 24 24"><path d="M4 17a8 8 0 1 1 16 0"/><path d="M12 17l4-5"/></symbol>
<symbol id="i-batt" viewBox="0 0 24 24"><rect x="2" y="7" width="18" height="10" rx="2"/><path d="M22 11v2M5 10v4"/></symbol>
<symbol id="i-wifi" viewBox="0 0 24 24"><path d="M2 9a15 15 0 0 1 20 0M5.5 12.5a10 10 0 0 1 13 0M9 16a5 5 0 0 1 6 0"/><path d="M12 19.5h.01"/></symbol>
<symbol id="i-chip" viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4M10 10h4v4h-4z"/></symbol>
<symbol id="i-db" viewBox="0 0 24 24"><ellipse cx="12" cy="5.5" rx="8" ry="3"/><path d="M4 5.5v13c0 1.7 3.6 3 8 3s8-1.3 8-3v-13M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3"/></symbol>
<symbol id="i-down" viewBox="0 0 24 24"><path d="M12 3v12M7 10l5 5 5-5M4 20h16"/></symbol>
<symbol id="i-up" viewBox="0 0 24 24"><path d="M12 15V3M7 8l5-5 5 5M4 20h16"/></symbol>
<symbol id="i-trash" viewBox="0 0 24 24"><path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/></symbol>
<symbol id="i-play" viewBox="0 0 24 24"><path d="M7 4.5l12 7.5-12 7.5z"/></symbol>
<symbol id="i-stop" viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="1.5"/></symbol>
<symbol id="i-check" viewBox="0 0 24 24"><path d="M4 12.5l5 5L20 6.5"/></symbol>
<symbol id="i-x" viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></symbol>
<symbol id="i-moon" viewBox="0 0 24 24"><path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/></symbol>
<symbol id="i-user" viewBox="0 0 24 24"><circle cx="12" cy="8" r="4"/><path d="M4 21c1-4 4-6 8-6s7 2 8 6"/></symbol>
<symbol id="i-reset" viewBox="0 0 24 24"><path d="M4 4v6h6"/><path d="M4.6 10A8 8 0 1 1 6 17.3"/></symbol>
<symbol id="i-list" viewBox="0 0 24 24"><path d="M9 6h11M9 12h11M9 18h11M4 6h.01M4 12h.01M4 18h.01"/></symbol>
<symbol id="i-gear" viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.3 5.3l2.1 2.1M16.6 16.6l2.1 2.1M5.3 18.7l2.1-2.1M16.6 7.4l2.1-2.1"/></symbol>
<symbol id="i-target" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5"/><path d="M12 12h.01"/></symbol>
<symbol id="i-clip" viewBox="0 0 24 24"><rect x="5" y="4" width="14" height="17" rx="2"/><path d="M9 4V2.5h6V4M8.5 10h7M8.5 14h7M8.5 18h4"/></symbol>
<symbol id="i-plus" viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></symbol>
<symbol id="i-next" viewBox="0 0 24 24"><path d="M5 5l8 7-8 7M14 5l5 7-5 7"/></symbol>
</svg>

<header>
<svg class="ico lg"><use href="#i-logo"/></svg>
<div><h1>Wireless Polygraph</h1><small id="dev">กำลังเชื่อมต่อนาฬิกา...</small></div>
<div class="hs"><span id="hbat"></span><span><i class="dot" id="dot"></i><b class="t" id="hcon">ออฟไลน์</b></span></div>
</header>
<nav><a href="#sTest">ถาม</a><a href="#sQ">ชุดคำถาม</a><a href="#sLive">ค่าสด</a><a href="#sHist">ผลย้อนหลัง</a><a href="#sData">ข้อมูลเทรน</a><a href="#sAI">โมเดล AI</a><a href="#sPow">พลังงาน</a></nav>

<main>
<section class="card">
<div class="seg" id="seg"><button data-m="train">เก็บข้อมูลเทรน AI</button><button data-m="detect">ใช้งานจริง</button></div>
<p class="hint" id="mhint"></p>
<ol class="steps" id="steps"></ol>
</section>

<section class="card" id="sRes">
<div class="lbl">ผลการตรวจล่าสุด</div>
<div class="vd" id="vd">-</div>
<div class="bar"><i id="vbar"></i></div>
<div class="meta" id="vmeta"></div>
<div id="vsig"></div>
<div id="vfb"></div>
</section>

<section class="card" id="sTest">
<h2><svg class="ico"><use href="#i-clip"/></svg>ควบคุมการทดสอบ</h2>
<div class="state"><svg class="ico"><use href="#i-target"/></svg><span id="st">-</span></div>
<div class="bar" style="margin-top:6px"><i id="sbar"></i></div>
<div id="ready"></div>
<div class="row one"><button id="bBase"><svg class="ico"><use href="#i-target"/></svg>วัดค่าปกติ (baseline)</button></div>
<div class="subj"><input type="text" id="subj" maxlength="20" placeholder="ชื่อผู้ถูกทดสอบ (ภาษาอังกฤษ)"><button id="bSubj"><svg class="ico"><use href="#i-user"/></svg>บันทึก</button></div>

<div id="trainBox">
<div class="row">
<button class="tru" id="bAskT"><svg class="ico"><use href="#i-check"/></svg>ถามข้อที่ให้ตอบตามจริง</button>
<button class="lie" id="bAskL"><svg class="ico"><use href="#i-x"/></svg>ถามข้อที่สั่งให้โกหก</button>
</div>
</div>

<div id="detBox">
<div class="subj"><input type="text" id="qtext" maxlength="120" placeholder="พิมพ์คำถาม (ไม่บังคับ ไว้ดูย้อนหลัง)"></div>
<div class="row one"><button class="pri" id="bAsk"><svg class="ico"><use href="#i-play"/></svg>เริ่มคำถาม</button></div>
<div class="row">
<button id="bYes"><svg class="ico"><use href="#i-check"/></svg>ตอบว่า "ใช่"</button>
<button id="bNo"><svg class="ico"><use href="#i-x"/></svg>ตอบว่า "ไม่ใช่"</button>
</div>
</div>

<div class="row">
<button id="bAbort"><svg class="ico"><use href="#i-stop"/></svg>ยกเลิกข้อนี้</button>
<button id="bReset"><svg class="ico"><use href="#i-reset"/></svg>เปลี่ยนผู้ถูกทดสอบ</button>
</div>
<div class="msg" id="msg"></div>
</section>

<section class="card" id="sQ">
<h2><svg class="ico"><use href="#i-list"/></svg>ชุดคำถาม<span class="r" id="qn"></span></h2>
<p class="hint" style="margin-top:0">เตรียมคำถามไว้ล่วงหน้าแล้วกด "ถามข้อถัดไป" ต่อกันได้เลย — วัดค่าปกติครั้งเดียวต่อคน ไม่ต้องวัดใหม่ทุกข้อ
ถ้ามีข้อควบคุม (ตอบจริง 1 + สั่งให้โกหก 1) ระบบจะปรับเกณฑ์ตามคนนั้นให้</p>
<div class="subj"><select id="tpl"><option value="classroom">สาธิตในห้องเรียน (แนะนำ)</option><option value="card">เกมทายเลขลับ 1-5</option>
<option value="quick">ทดสอบเร็ว 4 ข้อ</option><option value="custom">กำหนดเองทั้งหมด</option></select>
<button id="bTpl">ใช้ชุดนี้</button></div>
<div id="qlist"></div>
<div id="cit"></div>
<div class="row one"><button class="pri" id="bNext"><svg class="ico"><use href="#i-next"/></svg>ถามข้อถัดไป</button></div>
<div class="subj"><select id="qkind" style="flex:0 0 auto"><option value="test">คำถามจริง</option><option value="truth">ควบคุม: ตอบจริง</option>
<option value="lie">ควบคุม: สั่งให้โกหก</option><option value="warmup">อุ่นเครื่อง</option></select>
<input type="text" id="qnew" maxlength="120" placeholder="เพิ่มคำถาม (ตอบ ใช่/ไม่ใช่)"><button id="bQAdd"><svg class="ico"><use href="#i-plus"/></svg></button></div>
</section>

<section class="card" id="sLive">
<h2><svg class="ico"><use href="#i-pulse"/></svg>ค่าที่วัดได้ขณะนี้</h2>
<div class="grid" id="tiles"></div>
<div class="tabs" id="tabs"><button data-s="hr" class="on">ชีพจร</button><button data-s="gsr">GSR</button><button data-s="tmp">อุณหภูมิผิว</button><button data-s="trm">การสั่น</button></div>
<canvas id="cv"></canvas>
</section>

<section class="card" id="sHist">
<h2><svg class="ico"><use href="#i-list"/></svg>ผลย้อนหลัง<span class="r">24 ข้อล่าสุดในนาฬิกา</span></h2>
<div class="cnt four" id="acc"></div>
<div class="tw"><table><thead><tr><th>ข้อ</th><th>คำถาม</th><th>ผล</th><th>P</th><th>ถูก/ผิด</th></tr></thead><tbody id="res"></tbody></table></div>
<p class="hint">ถูก/ผิด = ผู้ใช้บอกหลังได้ผล (โหมดใช้งานจริง) ข้อที่บอกแล้วถูกบันทึกเป็นข้อมูลเทรนในนาฬิกา · ตัวเลขความแม่นยำนับเฉพาะในมือถือเครื่องนี้</p>
</section>

<section class="card" id="sData">
<h2><svg class="ico"><use href="#i-db"/></svg>ข้อมูลเทรนในนาฬิกา<span class="r">/train.csv</span></h2>
<div class="cnt"><div><b id="nT">0</b><span>ข้อตอบจริง</span></div><div><b id="nL">0</b><span>ข้อโกหก</span></div><div><b id="nB">0</b><span>ขนาด (KB)</span></div></div>
<div class="row">
<a class="btn" id="dl" href="/api/ml/data.csv" download="polygraph_train.csv"><svg class="ico"><use href="#i-down"/></svg>ดาวน์โหลด CSV</a>
<button id="bShow"><svg class="ico"><use href="#i-list"/></svg>แสดงรายการ</button>
</div>
<div id="dbox" class="hide">
<div class="row"><button id="bUndo"><svg class="ico"><use href="#i-reset"/></svg>ลบข้อล่าสุด</button>
<button class="dng" id="bClr"><svg class="ico"><use href="#i-trash"/></svg>ล้างทั้งหมด</button></div>
<div class="tw" style="max-height:340px;overflow-y:auto;margin-top:8px"><table><thead><tr><th>#</th><th>เวลา</th><th>ผู้ตอบ</th><th>qid</th><th>เฉลย</th><th>P</th><th></th></tr></thead><tbody id="drows"></tbody></table></div>
</div>
<p class="hint">ไฟล์ที่ดาวน์โหลดชื่อ polygraph_train_&lt;วันเวลา&gt;.csv — นำไปที่ Studio บนคอม หน้า "ข้อมูล &amp; เทรน AI" กด "นำเข้าไฟล์ CSV (จากมือถือ)"
(ที่มาจะถูกบันทึกเป็น mobile) หรือให้ Studio กด "ดึงข้อมูลที่นาฬิกาบันทึกเอง" ตอนต่อ WiFi นาฬิกา (ที่มา = esp_backup) — ระบบตัดข้อซ้ำให้</p>
</section>

<section class="card" id="sAI">
<h2><svg class="ico"><use href="#i-chip"/></svg>โมเดล AI</h2>
<div id="mlInfo"></div>
<div class="row">
<label class="btn" for="mf"><svg class="ico"><use href="#i-up"/></svg>อัปโหลด model.json</label>
<button class="dng" id="bMClr"><svg class="ico"><use href="#i-trash"/></svg>ลบโมเดล</button>
</div>
<input type="file" id="mf" accept=".json,application/json" class="hide">
<p class="hint">ถ้าต่อ Studio บนคอมไว้ Studio จะส่ง model.json ตัวล่าสุดให้อัตโนมัติ ไม่ต้องอัปโหลดเอง</p>
</section>

<section class="card" id="sPow">
<h2><svg class="ico"><use href="#i-gear"/></svg>พลังงานและการเชื่อมต่อ</h2>
<div class="kv"><span><svg class="ico"><use href="#i-moon"/></svg> หลับอัตโนมัติเมื่อไม่มีใครใช้</span>
<select id="sby"><option value="0">ปิด (ค่าเริ่มต้น)</option><option value="5">5 นาที</option><option value="10">10 นาที</option><option value="30">30 นาที</option><option value="60">60 นาที</option></select></div>
<div class="kv"><span><svg class="ico"><use href="#i-gauge"/></svg> โหมดประหยัดไฟ (ECO)</span>
<select id="eco"><option value="0">ปิด (CPU 160 MHz)</option><option value="1">เปิด (CPU 80 MHz)</option></select></div>
<div class="kv"><span><svg class="ico"><use href="#i-wifi"/></svg> กำลังส่ง WiFi</span>
<select id="wp"><option value="0">ต่ำ (แนะนำเมื่อใช้แบต)</option><option value="1">กลาง</option><option value="2">สูง</option></select></div>
<div class="row">
<button id="bLight"><svg class="ico"><use href="#i-moon"/></svg>พักเครื่องตอนนี้</button>
<button id="bDeep"><svg class="ico"><use href="#i-moon"/></svg>หลับลึก 60 วินาที</button>
</div>
<p class="hint">พักเครื่อง (light sleep): WiFi จะหลุด กดปุ่ม BOOT บนนาฬิกาเพื่อปลุก ค่าปกติที่วัดไว้ยังอยู่ครบ |
หลับลึก (deep sleep): ปิดเกือบทั้งหมด ตื่นเองเมื่อครบเวลาแล้วบูตใหม่ ต้องวัดค่าปกติใหม่</p>
</section>

<footer>FW <span id="fw">-</span> | <a href="/api/system" target="_blank">ข้อมูลระบบ (JSON)</a> | <a href="/update">อัปเดตเฟิร์มแวร์ (OTA)</a><br>
โครงงานรายวิชา 03603323 Introduction to Embedded Systems · ภาควิชาวิศวกรรมคอมพิวเตอร์ มก. ศรีราชา<br>
ผู้จัดทำ: อัจฉรา ดังดี 6730300655 · ปภากร จันทร์ดี 6730300809</footer>
</main>

<script>
'use strict';
const $=id=>document.getElementById(id);
const ic=n=>'<svg class="ico"><use href="#i-'+n+'"/></svg>';
const esc=s=>String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const fx=(v,d)=>(v==null||isNaN(v))?'--':Number(v).toFixed(d);
const VTH={truth:'จริง',lie:'โกหก',inconclusive:'ไม่แน่ชัด',invalid:'ใช้ไม่ได้',none:'-'};
const KTH={test:'คำถามจริง',truth:'ควบคุม: ตอบจริง',lie:'ควบคุม: โกหก',warmup:'อุ่นเครื่อง'};
const FTH={correct:'ถูก',wrong:'ผิด',unknown:'ไม่ทราบ'};
const SRC=['สูตรมาตรฐาน','สูตรปรับจากข้อควบคุม','โมเดล AI'];
const SIG={hr:['bpm',0],gsr:['µS',2],tmp:['°C',1],trm:['m/s²',3]};
// 5 สัญญาณที่ใช้ตัดสิน (ลำดับเดียวกับ feat/z ใน /api/lie)
const SN=['เหงื่อ (GSR)','ชีพจรเร็วขึ้น','แรงชีพจรลดลง','มือสั่น','ผิวเย็นลง'];
const STEPS={
 train:['ใส่นาฬิกาให้เซนเซอร์ชีพจรแนบผิว นั่งนิ่ง ๆ','กด "วัดค่าปกติ" ครั้งเดียวต่อคน (ประมาณ 30 วินาที)',
  'กด "ตอบตามจริง" หรือ "สั่งให้โกหก" (หรือใช้ชุดคำถามด้านล่าง) แล้วถามทันที ผลออกใน 12 วินาที',
  'ถามต่อกันได้เลยไม่ต้องวัดค่าปกติใหม่ ทำอย่างละ 15-20 ข้อ (หลายคนยิ่งดี) แล้วดาวน์โหลด CSV ไปเทรนบนคอม'],
 detect:['ใส่นาฬิกาให้เซนเซอร์ชีพจรแนบผิว นั่งนิ่ง ๆ','กด "วัดค่าปกติ" ครั้งเดียวต่อคน',
  'กด "เริ่มคำถาม" หรือ "ถามข้อถัดไป" แล้วถามทันที เมื่อผู้ถูกทดสอบตอบ ให้กด "ใช่" หรือ "ไม่ใช่"',
  'ได้ผลแล้วกดบอกว่านาฬิกาตอบถูกหรือผิด — ข้อที่บอกจะถูกเก็บเป็นข้อมูลเทรนในนาฬิกา']};
// ชุดคำถามสำเร็จรูป (ชุดเดียวกับ Studio บนคอม) {n} = ชื่อผู้ถูกทดสอบ
const TPL={
 classroom:[['warmup','ตอนนี้คุณนั่งอยู่บนเก้าอี้ใช่ไหม?'],['truth','คุณชื่อ {n} ใช่ไหม?'],['lie','คุณอายุ 50 ปีใช่ไหม?'],
  ['truth','วันนี้คุณมามหาวิทยาลัยใช่ไหม?'],['lie','คุณเป็นนักบินอวกาศใช่ไหม?'],['test','เมื่อคืนคุณนอนก่อนเที่ยงคืนใช่ไหม?'],
  ['test','คุณเคยลอกการบ้านเพื่อนใช่ไหม?'],['test','คุณชอบวิชา Embedded Systems ใช่ไหม?']],
 card:[['warmup','คุณเลือกเลขไว้ในใจแล้วใช่ไหม?'],['truth','คุณชื่อ {n} ใช่ไหม?'],['lie','คุณอายุ 50 ปีใช่ไหม?']]
  .concat([1,2,3,4,5].map(n=>['test','เลขที่คุณเลือกคือเลข '+n+' ใช่ไหม?','card'])),
 quick:[['truth','คุณชื่อ {n} ใช่ไหม?'],['lie','คุณมีพี่น้อง 9 คนใช่ไหม?'],['test','คุณเตรียมตัวสอบมาอย่างดีใช่ไหม?'],['test','คุณเคยแอบกินขนมของเพื่อนใช่ไหม?']],
 custom:[]};
let L=null,M=null,cfg={baselineSec:30,windowSec:12},lastRs=-1,lastMd=-1,sig='hr',fails=0,R=[],D=null;
const H={hr:[],gsr:[],tmp:[],trm:[]};

// ---------- สถานะที่จำไว้ในมือถือ (localStorage: ปิดหน้าแล้วเปิดใหม่ยังอยู่) ----------
// qs  = ชุดคำถาม [{k:ชนิด,t:ข้อความ,g:กลุ่ม,q:qid,a:seqก่อนถาม,r:{seq,v,p,sc}}]
// fb  = ถูก/ผิดที่ผู้ใช้บอก {บูต:seq: correct|wrong|unknown}, tx = ข้อความคำถาม {qid: text}
let ST={qs:[],fb:{},tx:{},nq:1000},BOOT='';
// กุญแจของผล 1 ข้อ = รอบบูต:seq (seq เริ่มนับใหม่ทุกครั้งที่นาฬิการีบูต จึงใช้ seq อย่างเดียวไม่ได้)
const fk=r=>BOOT+':'+r.seq;
async function loadBoot(){try{const i=await api('/api/info');BOOT=String(i.bootCount==null?'':i.bootCount);paintRes();}catch(e){}}
function load(){try{const s=JSON.parse(localStorage.getItem('pg2')||'null');if(s&&s.qs)ST=Object.assign(ST,s);}catch(e){}}
function save(){try{localStorage.setItem('pg2',JSON.stringify(ST));}catch(e){}}
// qid ของคำถามจากมือถือ 1000-59999 (ไม่ชนช่วงที่ Studio/ปุ่มใช้: 1-999)
function newQid(){const q=ST.nq;ST.nq=ST.nq>=59999?1000:ST.nq+1;
 const k=Object.keys(ST.tx);if(k.length>400)k.slice(0,100).forEach(x=>delete ST.tx[x]);   // ไม่ให้ localStorage โตไม่สิ้นสุด
 return q;}

// ---------- การเรียก API ----------
// fetch แล้วแปลงเป็น JSON; ถ้า HTTP ไม่ใช่ 2xx หรือ {"ok":false} ให้ throw ข้อความไทยจากนาฬิกา
async function api(p,o){
 const r=await fetch(p,o);let j=null;
 try{j=await r.json()}catch(e){}
 if(!r.ok||(j&&j.ok===false))throw new Error(j&&j.msg?j.msg:'HTTP '+r.status);
 return j||{};
}
function post(p,body){
 return api(p,body?{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}:{method:'POST'});
}
function note(t,bad){
 const m=$('msg');m.textContent=t;m.className='msg '+(bad?'bad':'ok');
 clearTimeout(note.t);note.t=setTimeout(()=>{m.textContent=''},8000);
 if(bad&&m.getBoundingClientRect().top>innerHeight)alert(t);
}
async function act(p,ok){
 let j=null;
 try{j=await post(p);note(ok||j.msg||'เรียบร้อย');}
 catch(e){note(e.message,true);}
 live(false);loadRes();loadMl();
 return j;
}

// ---------- ค่าสด (ทุก 1 วินาที) ----------
async function live(push){
 try{
  L=await api('/api/live');fails=0;
  if(!BOOT)await loadBoot();
  if(push){
   const v={hr:L.con?L.hr:null,gsr:L.gc?L.gsr:null,tmp:L.tmp,trm:L.trm};
   for(const k in H){H[k].push(v[k]==null?null:+v[k]);if(H[k].length>120)H[k].shift();}
  }
  render();chart();
  if(L.rs!==lastRs){lastRs=L.rs;loadRes();loadMl();}
 }catch(e){
  if(++fails>=3){BOOT='';$('dot').className='dot';$('hcon').textContent='ออฟไลน์';
   $('dev').textContent='ขาดการเชื่อมต่อ - ตรวจว่ายังต่อ WiFi "Polygraph-Watch" อยู่';}
 }
}
function tile(icon,name,val,unit,sub,off){
 return '<div class="tile'+(off?' off':'')+'"><div class="h">'+ic(icon)+name+'</div><b>'+val+
  (off?'':' <small>'+unit+'</small>')+'</b><em>'+(sub||'')+'</em></div>';
}
function chk(ok,t1,t2,warn){return '<div class="chk"><b class="'+(ok?'ok':warn?'wn':'')+'">'+(ok?'&#10003;':warn?'!':'&times;')+'</b><span>'+(ok?t1:t2)+'</span></div>';}
function render(){
 $('dot').className='dot on';$('hcon').textContent='เชื่อมต่อแล้ว';
 $('dev').textContent=L.id+'  |  FW '+L.fw;$('fw').textContent=L.fw;
 $('hbat').innerHTML=ic('batt')+(L.bat?L.bp+'%':'USB');
 $('hbat').title='แบตเตอรี่ '+fx(L.vb/1000,2)+' V';

 const train=L.md===1;
 if(L.md!==lastMd){
  lastMd=L.md;
  document.querySelectorAll('#seg button').forEach(b=>b.classList.toggle('on',(b.dataset.m==='train')===train));
  $('trainBox').classList.toggle('hide',!train);$('detBox').classList.toggle('hide',train);
  $('steps').innerHTML=STEPS[train?'train':'detect'].map(s=>'<li>'+s+'</li>').join('');
  paintRes();
 }
 $('mhint').textContent=train
  ?'โหมดเก็บข้อมูล: ข้อควบคุม (ตอบจริง/สั่งให้โกหก) ทุกข้อถูกบันทึกลงไฟล์ CSV ในนาฬิกา เพื่อนำไปเทรน AI'
  :'โหมดใช้งานจริง: ถามคำถามที่ไม่รู้เฉลย ระบบตัดสินด้วย'+(L.ml?'โมเดล AI ที่ติดตั้งไว้':'สูตรมาตรฐาน (ยังไม่ได้ติดตั้งโมเดล AI)')+' แล้วกดบอกว่าถูก/ผิด';

 // สถานะ engine: 0 ยังไม่วัด, 1 กำลังวัดค่าปกติ, 2 พร้อม, 3 กำลังถาม
 const es=L.es,p=Math.round((L.ep||0)*100);
 let st='ขั้นแรก: กด "วัดค่าปกติ"';
 if(es===1)st='กำลังวัดค่าปกติ เหลือ '+Math.max(0,Math.ceil((1-L.ep)*cfg.baselineSec))+' วินาที - นั่งนิ่ง ๆ หายใจตามปกติ';
 else if(es===3)st='กำลังวัดคำตอบ'+(ST.tx[L.eq]?' "'+ST.tx[L.eq]+'"':'')+' เหลือ '+Math.max(0,Math.ceil((1-L.ep)*cfg.windowSec))+' วินาที';
 else if(es===2)st=L.set?'พร้อมถามข้อถัดไป (ไม่ต้องวัดค่าปกติใหม่)':'รอให้ร่างกายกลับสู่ปกติสักครู่ แล้วค่อยถามข้อถัดไป';
 $('st').textContent=st;
 $('sbar').style.width=(es===1||es===3?p:es===2?100:0)+'%';
 $('ready').innerHTML=chk(L.bl,'วัดค่าปกติแล้ว — ใช้ต่อได้ทุกข้อจนกว่าจะเปลี่ยนคน','ยังไม่ได้วัดค่าปกติ')+
  chk(L.cal,'ปรับเกณฑ์ตามผู้ถูกทดสอบแล้ว (จากข้อควบคุม)','ยังไม่ได้ปรับเกณฑ์ — ถามข้อควบคุม ตอบจริง 1 + โกหก 1 ข้อ (ไม่บังคับ)',true)+
  (L.bl?chk(L.set,'สัญญาณนิ่ง พร้อมถาม','รอสัญญาณนิ่ง 10-20 วินาที (ถามเลยได้ แต่ผลอาจคลาดเคลื่อน)',true):'');
 const ready=es===2,q=es===3,base=es===1;
 $('bBase').disabled=base||q;
 $('bBase').innerHTML=ic('target')+(L.bl?'วัดค่าปกติใหม่ (เปลี่ยนคน/ถอดนาฬิกา)':'วัดค่าปกติ (baseline)');
 ['bAskT','bAskL','bAsk'].forEach(i=>{$(i).disabled=!ready});
 ['bYes','bNo'].forEach(i=>{$(i).disabled=!q});
 $('bAbort').disabled=!(q||base);
 $('bNext').disabled=!ready||!nextQ();
 document.querySelectorAll('#qlist button[data-a]').forEach(b=>{b.disabled=!ready});
 const cur=q?ST.qs.findIndex(x=>x.q===L.eq):-1;
 document.querySelectorAll('#qlist .q').forEach((e,i)=>e.classList.toggle('cur',i===cur));

 const hrOk=L.con&&L.hr!=null;
 $('tiles').innerHTML=
  tile('heart','ชีพจร',hrOk?fx(L.hr,0):(L.con?'กำลังจับ...':'ไม่แตะผิว'),'bpm',hrOk?'HRV '+fx(L.hrv,0)+' ms':'',!hrOk)+
  tile('drop','GSR (ความชื้นผิว)',L.gc?fx(L.gsr,2):'ไม่มีสัญญาณ','µS',L.gc?'':'ใช้ค่าอื่นแทนได้',!L.gc)+
  tile('temp','อุณหภูมิผิว',fx(L.tmp,1),'°C','',L.tmp==null)+
  tile('wave','มือสั่น',fx(L.trm,3),'m/s²','ขยับตัว '+fx(L.mot,2),false)+
  tile('gauge','ระดับความตื่นตัว',L.si>=0?L.si:'--','/ 100',L.si>=0?'':'คำนวณหลังวัดค่าปกติ',false)+
  tile('batt','แบตเตอรี่',L.bat?L.bp:'USB',L.bat?'%':'',fx(L.vb/1000,2)+' V | CPU '+L.cpu+' MHz',false);

 for(const [id,v] of [['sby',L.sby],['eco',L.eco],['wp',L.wp]]){
  const s=$(id);if(document.activeElement===s||v==null)continue;
  if(![...s.options].some(o=>+o.value===+v)){const o=document.createElement('option');o.value=v;o.textContent=v+' นาที';s.appendChild(o);}
  s.value=String(v);
 }
}

// ---------- กราฟ 2 นาทีล่าสุด ----------
function chart(){
 const c=$('cv'),d=window.devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
 if(!w)return;
 if(c.width!==Math.round(w*d)){c.width=Math.round(w*d);c.height=Math.round(h*d);}
 const g=c.getContext('2d');g.setTransform(d,0,0,d,0,0);g.clearRect(0,0,w,h);
 const a=H[sig],ok=a.filter(v=>v!=null);
 g.font='11px system-ui,sans-serif';g.fillStyle='#5f6b80';
 if(ok.length<2){g.fillText(sig==='gsr'?'ไม่มีสัญญาณ GSR (ผิวแห้งหรือแผ่นไม่แตะ)':'ยังไม่มีข้อมูล',12,22);return;}
 let lo=Math.min(...ok),hi=Math.max(...ok);
 if(hi-lo<1e-6){hi+=0.5;lo-=0.5;}
 const pd=(hi-lo)*0.12;lo-=pd;hi+=pd;
 const L0=46,T0=8,W=w-L0-8,Hh=h-T0-22,X=i=>L0+W*i/119,Y=v=>T0+Hh*(hi-v)/(hi-lo);
 // ช่วงแคบ (เช่นชีพจรแกว่ง 2 bpm) ต้องเพิ่มทศนิยม ไม่งั้นเลขแกนซ้ำกัน (76, 76)
 const dp=Math.max(SIG[sig][1],Math.min(3,Math.ceil(-Math.log10((hi-lo)/3))));
 g.strokeStyle='#e5e9f0';g.lineWidth=1;
 for(let k=0;k<=3;k++){const y=T0+Hh*k/3;g.beginPath();g.moveTo(L0,y);g.lineTo(w-8,y);g.stroke();
  g.fillText((hi-(hi-lo)*k/3).toFixed(dp),4,y+4);}
 g.strokeStyle='#173f73';g.lineWidth=2;g.beginPath();
 let pen=false;const off=120-a.length;
 a.forEach((v,i)=>{if(v==null){pen=false;return;}const x=X(i+off),y=Y(v);if(pen)g.lineTo(x,y);else g.moveTo(x,y);pen=true;});
 g.stroke();g.fillText('2 นาทีล่าสุด  หน่วย '+SIG[sig][0],L0,h-6);
}

// ---------- ผลการตรวจ ----------
async function loadRes(){
 try{
  const j=await api('/api/lie?n=24');
  if(j.config)cfg=j.config;
  R=j.results||[];
  matchQs();paintRes();
 }catch(e){}
}
// จับคู่ผลกับข้อในชุดคำถาม: qid ตรงกัน และ seq ใหม่กว่าตอนกดถาม (กันผลเก่าของ qid เดิม)
function matchQs(){
 let ch=false;
 for(const q of ST.qs){
  if(q.r||q.a==null)continue;
  if(L&&L.rs<q.a)q.a=0;                 // นาฬิการีบูตระหว่างรอผล -> seq เริ่มนับใหม่
  const r=R.find(x=>x.qid===q.q&&x.seq>q.a);
  if(r){q.r={seq:r.seq,v:r.verdict,p:r.p,sc:r.score};ch=true;}
 }
 if(ch){save();paintQ();}
}
function reasons(m){
 const t=[];
 if(m&1)t.push('ไม่มีสัญญาณ GSR (ใช้ค่าอื่นแทน)');
 if(m&2)t.push('เซนเซอร์ชีพจรไม่แตะผิว');
 if(m&4)t.push('ขยับตัวมากเกินไป');
 if(m&8)t.push('ข้อมูลก่อนถามไม่พอ (ถามติดกันเร็วไป)');
 return t;
}
// ข้อนี้ให้ผู้ใช้บอกถูก/ผิดได้ไหม: คำถามจริง (ไม่รู้เฉลย) ที่สัญญาณใช้ได้
const canFb=r=>r&&r.kind==='test'&&r.verdict!=='invalid';
function paintRes(){
 showVerdict(R[0]);
 const k=fbCount();
 $('acc').innerHTML=[['ถูก',k.correct,'var(--tru)'],['ผิด',k.wrong,'var(--lie)'],['ไม่ทราบ',k.unknown,'var(--inc)'],
  ['แม่นยำ',k.correct+k.wrong?Math.round(100*k.correct/(k.correct+k.wrong))+'%':'-','var(--pri)']]
  .map(([t,v,c])=>'<div><b style="color:'+c+'">'+v+'</b><span>'+t+'</span></div>').join('');
 $('res').innerHTML=R.length?R.map(r=>{
  const f=ST.fb[fk(r)];
  const fbc=f?FTH[f]:canFb(r)?'<button class="sm" data-f="'+r.seq+'">บอกผล</button>':'-';
  return '<tr><td>'+r.qid+'</td><td>'+esc(ST.tx[r.qid]||KTH[r.kind]||r.kind)+'</td><td class="'+r.verdict+'">'+
   (VTH[r.verdict]||r.verdict)+'</td><td>'+Math.round(r.p*100)+'%</td><td>'+fbc+'</td></tr>';}).join('')
  :'<tr><td colspan="5" style="color:var(--mut)">ยังไม่มีผล</td></tr>';
 document.querySelectorAll('#res button[data-f]').forEach(b=>{b.onclick=()=>{
  const r=R.find(x=>x.seq===+b.dataset.f);showVerdict(r,true);$('sRes').scrollIntoView({behavior:'smooth'});}});
}
function fbCount(){const k={correct:0,wrong:0,unknown:0};for(const s in ST.fb)if(k[ST.fb[s]]!=null)k[ST.fb[s]]++;return k;}
function showVerdict(r,force){
 const v=$('vd'),b=$('vbar');
 if(!r){v.textContent='-';v.className='vd';b.style.width='0';$('vsig').innerHTML='';$('vfb').innerHTML='';
  $('vmeta').innerHTML='<span class="tag">ยังไม่มีผล - วัดค่าปกติแล้วเริ่มคำถาม</span>';return;}
 v.textContent=VTH[r.verdict]||r.verdict;v.className='vd '+r.verdict;
 const p=Math.round(r.p*100);b.style.width=p+'%';
 b.style.background=r.verdict==='lie'?'var(--lie)':r.verdict==='truth'?'var(--tru)':'var(--inc)';
 let t='<span class="tag">ข้อ '+r.qid+' | '+(KTH[r.kind]||r.kind)+'</span><span class="tag">โอกาสโกหก '+p+'%</span>'+
  '<span class="tag">ตัดสินด้วย'+(SRC[r.src]||'-')+'</span><span class="tag">คุณภาพสัญญาณ '+r.quality+'%</span>';
 if(ST.tx[r.qid])t='<span class="tag">"'+esc(ST.tx[r.qid])+'"</span>'+t;
 if(r.kind==='truth'||r.kind==='lie'){
  t+='<span class="tag">เฉลย: '+(r.kind==='lie'?'โกหก':'จริง')+(r.verdict==='invalid'?'':(r.correct?' (ระบบทายถูก)':' (ระบบทายผิด)'))+'</span>';
  if(L&&L.md===1)t+='<span class="tag'+(r.verdict==='invalid'?' w':' t')+'">'+(r.verdict==='invalid'?'สัญญาณไม่ดี ไม่บันทึกข้อนี้':'บันทึกลงข้อมูลเทรนแล้ว')+'</span>';
 }
 for(const s of reasons(r.reasons))t+='<span class="tag w">'+s+'</span>';
 $('vmeta').innerHTML=t;
 // สัญญาณไหนเปลี่ยนมาก: z = เปลี่ยนไปกี่เท่าของความแกว่งปกติ (เต็มหลอด = 6 เท่า)
 const z=r.z||[],ok=r.ok||0;
 $('vsig').innerHTML='<div style="margin-top:10px"></div>'+SN.map((n,i)=>{
  const has=(ok>>i)&1&&z[i]!=null,zi=has?z[i]:0;
  const lv=!has?'ไม่มีข้อมูล':zi<1?'ปกติ':zi<2?'เล็กน้อย':zi<4?'ชัดเจน':'สูงมาก';
  return '<div class="sig"><span>'+n+'</span><div class="tr"><i style="width:'+Math.max(0,Math.min(100,zi/6*100))+
   '%;background:'+(zi>=2?'var(--lie)':'var(--pri)')+'"></i></div><span style="color:var(--mut)">'+lv+'</span></div>';}).join('');
 // ปุ่มบอกถูก/ผิด (เฉพาะคำถามจริงที่ยังไม่ได้บอก)
 const f=ST.fb[fk(r)];
 if(!canFb(r)){$('vfb').innerHTML='';return;}
 if(f&&!force){$('vfb').innerHTML='<div class="fb">ผู้ใช้บอกว่า: <b>'+FTH[f]+'</b>'+(f!=='unknown'?' — บันทึกเป็นข้อมูลเทรนในนาฬิกาแล้ว':'')+'</div>';return;}
 const sure=r.verdict==='truth'||r.verdict==='lie';
 $('vfb').innerHTML='<div class="fb"><b>'+(sure?'นาฬิกาตอบถูกไหม?':'นาฬิกาไม่แน่ใจ — ที่จริงผู้ถูกทดสอบพูดจริงหรือโกหก?')+'</b>'+
  '<div class="row three">'+(sure?'<button class="tru" data-v="correct">'+ic('check')+'ถูก</button><button class="lie" data-v="wrong">'+ic('x')+'ผิด</button>'
  :'<button class="tru" data-v="truth">พูดจริง</button><button class="lie" data-v="lie">โกหก</button>')+
  '<button data-v="unknown">ไม่ทราบ</button></div><p class="hint">ถูก/ผิด จะถูกบันทึกเป็นข้อมูลเทรนในนาฬิกา (ไม่ทราบ = ไม่บันทึก)</p></div>';
 $('vfb').querySelectorAll('button').forEach(b=>{b.onclick=()=>sendFb(r,b.dataset.v);});
}
// แปลงคำตอบของผู้ใช้เป็นเฉลย แล้วส่งให้นาฬิกาบันทึก: ทาย "จริง" + ถูก = จริง, ทาย "จริง" + ผิด = โกหก ...
async function sendFb(r,v){
 let label=null,fb=v;
 if(v==='correct')label=r.verdict;
 else if(v==='wrong')label=r.verdict==='lie'?'truth':'lie';
 else if(v==='truth'||v==='lie'){label=v;fb=v==='truth'?(r.verdict==='truth'?'correct':'wrong'):(r.verdict==='lie'?'correct':'wrong');}
 if(r.verdict==='inconclusive'&&label)fb='wrong';   // ไม่แน่ชัด = ไม่ได้ทายถูก
 if(label){
  try{const j=await post('/api/ml/feedback?seq='+r.seq+'&label='+label);note(j.msg);}
  catch(e){if(!/บันทึกไปแล้ว/.test(e.message)){note(e.message,true);return;}}
 }else note('ไม่บันทึกข้อนี้เป็นข้อมูลเทรน');
 ST.fb[fk(r)]=fb;
 const keys=Object.keys(ST.fb);if(keys.length>300)delete ST.fb[keys[0]];
 save();paintRes();loadMl();if(D)loadData();
}

// ---------- ชุดคำถาม ----------
function nextQ(){return ST.qs.find(q=>!q.r&&q.a==null)||ST.qs.find(q=>!q.r)||null;}
function useTpl(){
 const name=($('subj').value||'').trim()||'...';
 if(ST.qs.some(q=>q.r)&&!confirm('แทนที่ชุดคำถามเดิม? (ผลที่ได้แล้วยังอยู่ในตารางผลย้อนหลัง)'))return;
 ST.qs=TPL[$('tpl').value].map(([k,t,g])=>({k:k,t:t.replace('{n}',name),g:g||'',q:newQid(),a:null,r:null}));
 for(const q of ST.qs)ST.tx[q.q]=q.t;
 save();paintQ();if(L)render();
}
function paintQ(){
 $('qn').textContent=ST.qs.length?ST.qs.filter(q=>q.r).length+'/'+ST.qs.length+' ข้อ':'';
 $('qlist').innerHTML=ST.qs.length?ST.qs.map((q,i)=>{
  const r=q.r,kc=q.k==='lie'?' l':q.k==='truth'?' t':'';
  return '<div class="q'+(r?' done':'')+'"><span class="n">'+(i+1)+'</span><div><div class="tx">'+esc(q.t)+'</div><div class="sub">'+
   '<span class="tag'+kc+'">'+KTH[q.k]+'</span>'+(r?'<span class="tag'+(r.v==='lie'?' l':r.v==='truth'?' t':' w')+'">'+VTH[r.v]+' '+Math.round(r.p*100)+'%</span>':
   q.a!=null?'<span class="tag w">รอผล</span>':'')+'</div></div><div class="ac">'+
   '<button class="sm pri" data-a="'+i+'">'+(r?'ถามซ้ำ':'ถาม')+'</button>'+
   (r?'':'<button class="sm" data-d="'+i+'" title="ลบข้อนี้">'+ic('trash')+'</button>')+'</div></div>';}).join('')
  :'<p class="hint">ยังไม่มีคำถาม — เลือกชุดด้านบนแล้วกด "ใช้ชุดนี้" หรือพิมพ์เพิ่มเองด้านล่าง</p>';
 $('qlist').querySelectorAll('button[data-a]').forEach(b=>{b.onclick=()=>askQ(+b.dataset.a);});
 $('qlist').querySelectorAll('button[data-d]').forEach(b=>{b.onclick=()=>{ST.qs.splice(+b.dataset.d,1);save();paintQ();};});
 // เกมทายเลขลับ: ข้อที่ร่างกายตอบสนองแรงสุด (score สูงสุด) = เลขที่เลือก
 const g=ST.qs.filter(q=>q.g==='card'&&q.r&&q.r.v!=='invalid').sort((a,b)=>b.r.sc-a.r.sc);
 if(g.length>=2){
  const m=g[0].r.sc-g[1].r.sc,c=m>=1?'สูง':m>=0.5?'ปานกลาง':'ต่ำ';
  $('cit').innerHTML='<div class="fb">เกมทายเลขลับ ('+g.length+'/5 ข้อ): ระบบเดาว่า <b>"'+esc(g[0].t)+'"</b> — มั่นใจ'+c+'</div>';
 }else $('cit').innerHTML='';
 if(L)$('bNext').disabled=L.es!==2||!nextQ();
}
async function askQ(i){
 const q=ST.qs[i];if(!q)return;
 const prev=Math.max(0,...R.map(r=>r.seq),L&&L.rs?L.rs:0);   // ผลที่ seq <= ค่านี้ = ของเก่า
 try{
  await post('/api/lie/question?qid='+q.q+'&kind='+q.k);
  q.a=prev;q.r=null;ST.tx[q.q]=q.t;save();paintQ();
  note('ถามได้เลย: "'+q.t+'"'+(q.k==='lie'?' (สั่งให้ผู้ถูกทดสอบโกหก)':q.k==='truth'?' (ให้ตอบตามจริง)':''));
 }catch(e){note(e.message,true);}
 live(false);
}
function addQ(){
 const t=$('qnew').value.trim();if(!t)return note('พิมพ์คำถามก่อน',true);
 const q={k:$('qkind').value,t:t,g:'',q:newQid(),a:null,r:null};
 ST.qs.push(q);ST.tx[q.q]=t;$('qnew').value='';save();paintQ();
}

// ---------- ข้อมูลเทรนในนาฬิกา ----------
async function loadData(){
 try{
  if(M&&M.data&&!M.data.bytes)throw 0;            // ยังไม่มีไฟล์ -> ไม่ต้องขอ (กัน 404 ใน console)
  const r=await fetch('/api/ml/data.csv');
  if(r.status===404){D=[];}
  else{
   const lines=(await r.text()).split('\n').filter(x=>x.length);
   D=lines.slice(1).map((ln,i)=>{const f=ln.split(',');return {row:i+1,t:f[0],s:f[1],q:f[2],lb:f[4],p:f[18]};});
  }
 }catch(e){D=[];}
 const fmtT=t=>+t>1.6e9?new Date(t*1000).toLocaleString('th-TH',{dateStyle:'short',timeStyle:'short'}):'-';
 $('drows').innerHTML=D.length?D.slice().reverse().map(d=>'<tr><td>'+d.row+'</td><td>'+fmtT(d.t)+'</td><td>'+esc(d.s)+'</td><td>'+esc(d.q)+
  '</td><td class="'+d.lb+'">'+(d.lb==='lie'?'โกหก':'จริง')+'</td><td>'+(d.p?Math.round(d.p*100)+'%':'-')+'</td><td><button class="sm" data-r="'+d.row+'" title="ลบแถวนี้">'+ic('trash')+'</button></td></tr>').join('')
  :'<tr><td colspan="7" style="color:var(--mut)">ยังไม่มีข้อมูลเทรนในนาฬิกา</td></tr>';
 $('drows').querySelectorAll('button[data-r]').forEach(b=>{b.onclick=()=>delRow(D.find(d=>d.row===+b.dataset.r));});
 $('bUndo').disabled=!D.length;
}
async function delRow(d){
 if(!d||!confirm('ลบข้อมูลเทรนแถวที่ '+d.row+' (qid '+d.q+', เฉลย '+(d.lb==='lie'?'โกหก':'จริง')+')? ลบแล้วกู้คืนไม่ได้'))return;
 try{const j=await post('/api/ml/data/delete?row='+d.row+'&t='+encodeURIComponent(d.t)+'&qid='+encodeURIComponent(d.q));note(j.msg);}
 catch(e){note(e.message,true);}
 loadData();loadMl();
}

// ---------- AI ----------
async function loadMl(){
 try{
  M=await api('/api/ml');
  const changed=$('nT').textContent!=String(M.data.truth)||$('nL').textContent!=String(M.data.lie);
  $('nT').textContent=M.data.truth;$('nL').textContent=M.data.lie;$('nB').textContent=(M.data.bytes/1024).toFixed(1);
  if(changed&&D)loadData();
  if(document.activeElement!==$('subj'))$('subj').value=M.subject&&M.subject!=='-'?M.subject:'';
  const m=M.model;
  $('mlInfo').innerHTML=m.loaded
   ?'<div class="kv"><span>สถานะ</span><b style="color:var(--tru)">ติดตั้งแล้ว ใช้ตัดสินในโหมดใช้งานจริง</b></div>'+
    '<div class="kv"><span>ชื่อโมเดล</span><span>'+esc(m.name)+'</span></div>'+
    '<div class="kv"><span>ความแม่นยำจาก cross-validation</span><span>'+Math.round(m.accuracy*100)+'%'+(m.accuracy<0.6?' (ยังต่ำ ควรเก็บข้อมูลเพิ่ม)':'')+'</span></div>'+
    '<div class="kv"><span>จำนวนข้อมูลที่ใช้เทรน</span><span>'+m.samples+' ข้อ</span></div>'+
    '<div class="kv"><span>ค่าที่ใช้ตัดสิน</span><span style="text-align:right">'+esc(m.features.join(', '))+'</span></div>'
   :'<p class="hint" style="margin:0">ยังไม่ได้ติดตั้งโมเดล ตอนนี้ตัดสินด้วยสูตรมาตรฐาน<br>'+
    'ขั้นตอน: เก็บข้อมูลในโหมดเก็บข้อมูล &rarr; ดาวน์โหลด CSV &rarr; เทรนบนคอม (Studio หน้า ข้อมูล &amp; เทรน AI) &rarr; อัปโหลด model.json ที่นี่</p>';
  $('bMClr').disabled=!m.loaded;
 }catch(e){}
}
// อ่าน model.json ในมือถือแล้วส่งเป็นฟอร์มสั้น ๆ (นาฬิกาไม่ต้องมีตัวแปลง JSON)
$('mf').onchange=async e=>{
 const f=e.target.files[0];e.target.value='';if(!f)return;
 try{
  const m=JSON.parse(await f.text());
  if(!Array.isArray(m.features)||!Array.isArray(m.w)||!Array.isArray(m.mean)||!Array.isArray(m.scale))
   throw new Error('ไฟล์นี้ไม่ใช่ model.json จาก ml/train.py');
  const body=new URLSearchParams({features:m.features.join(','),mean:m.mean.join(','),scale:m.scale.join(','),
   w:m.w.join(','),b:String(m.b),acc:String(m.cv_accuracy||0),samples:String(m.samples||0),
   trained:String(m.trained_at||0),name:String(m.name||'model').slice(0,23)});
  const j=await post('/api/ml/model',body.toString());note(j.msg||'ติดตั้งโมเดลแล้ว');
 }catch(err){note(err.message,true);}
 loadMl();live(false);
};

// ---------- ปุ่ม ----------
document.querySelectorAll('#seg button').forEach(b=>{b.onclick=()=>act('/api/ml/mode?mode='+b.dataset.m)});
document.querySelectorAll('#tabs button').forEach(b=>{b.onclick=()=>{
 sig=b.dataset.s;document.querySelectorAll('#tabs button').forEach(x=>x.classList.toggle('on',x===b));chart();}});
$('bBase').onclick=()=>{if(L&&L.bl&&!confirm('วัดค่าปกติใหม่? ทำเฉพาะตอนเปลี่ยนผู้ถูกทดสอบหรือถอดนาฬิกา — ข้อควบคุมที่ถามไปแล้วต้องถามใหม่'))return;
 act('/api/lie/baseline','เริ่มวัดค่าปกติ - นั่งนิ่ง ๆ หายใจตามปกติ');};
// ถามด่วน (ไม่อยู่ในชุดคำถาม) — จำข้อความคำถามไว้กับ qid ที่นาฬิกาตอบกลับ
async function quick(kind,okText){
 const t=kind==='test'?$('qtext').value.trim():'';
 const q=newQid();save();
 const j=await act('/api/lie/question?qid='+q+'&kind='+kind,okText);
 if(j&&t){ST.tx[q]=t;$('qtext').value='';save();}
}
$('bAskT').onclick=()=>quick('truth','ถามได้เลย: ข้อนี้ให้ผู้ถูกทดสอบตอบตามความจริง');
$('bAskL').onclick=()=>quick('lie','ถามได้เลย: ข้อนี้ให้ผู้ถูกทดสอบโกหก');
$('bAsk').onclick=()=>quick('test','ถามคำถามได้เลย แล้วกดปุ่มตามคำตอบ');
$('bNext').onclick=()=>{const q=nextQ();if(q)askQ(ST.qs.indexOf(q));};
$('bTpl').onclick=useTpl;
$('bQAdd').onclick=addQ;
$('qnew').onkeydown=e=>{if(e.key==='Enter')addQ();};
$('bYes').onclick=()=>act('/api/lie/answer?ans=yes','บันทึกคำตอบ "ใช่" แล้ว');
$('bNo').onclick=()=>act('/api/lie/answer?ans=no','บันทึกคำตอบ "ไม่ใช่" แล้ว');
$('bAbort').onclick=()=>act('/api/lie/abort','ยกเลิกแล้ว');
$('bReset').onclick=()=>{if(confirm('เริ่มผู้ถูกทดสอบคนใหม่? ล้างค่าปกติ/ข้อควบคุม/ผลในหน่วยความจำ (ข้อมูลเทรนในไฟล์ CSV ไม่หาย)')){
 for(const q of ST.qs){q.a=null;q.r=null;}save();paintQ();act('/api/lie/reset','เริ่มใหม่แล้ว — วัดค่าปกติของคนใหม่');}};
$('bSubj').onclick=()=>act('/api/ml/subject?name='+encodeURIComponent($('subj').value.trim()),'บันทึกชื่อแล้ว');
$('bShow').onclick=()=>{const h=$('dbox').classList.toggle('hide');$('bShow').innerHTML=ic('list')+(h?'แสดงรายการ':'ซ่อนรายการ');if(!h)loadData();else D=null;};
$('bUndo').onclick=()=>{if(D&&D.length)delRow(D[D.length-1]);};
// ชื่อไฟล์ที่ดาวน์โหลดมีวันเวลา (ขึ้นต้น polygraph_train เสมอ — Studio ใช้ชื่อนี้บอกว่าข้อมูลมาจากมือถือ)
$('dl').onclick=()=>{const d=new Date(),p=n=>String(n).padStart(2,'0');
 $('dl').download='polygraph_train_'+d.getFullYear()+p(d.getMonth()+1)+p(d.getDate())+'_'+p(d.getHours())+p(d.getMinutes())+p(d.getSeconds())+'.csv';};
$('bClr').onclick=()=>{if(confirm('ลบข้อมูลเทรนทั้งหมดในนาฬิกา? ควรดาวน์โหลด CSV เก็บไว้ก่อน'))act('/api/ml/data/clear?confirm=yes').then(()=>{if(D)loadData();});};
$('bMClr').onclick=()=>{if(confirm('ลบโมเดล AI แล้วกลับไปใช้สูตรมาตรฐาน?'))act('/api/ml/model/clear');};
$('sby').onchange=e=>act('/api/sleep?auto='+e.target.value,e.target.value==='0'?'ปิดการหลับอัตโนมัติแล้ว':'นาฬิกาจะหลับเมื่อไม่มีใครใช้ '+e.target.value+' นาที');
$('eco').onchange=e=>act('/api/power?mode='+(e.target.value==='1'?'eco':'normal'));
$('wp').onchange=e=>act('/api/wifi?level='+e.target.value);
$('bLight').onclick=()=>{if(confirm('WiFi จะหลุดจนกว่าจะกดปุ่ม BOOT บนนาฬิกา ดำเนินการต่อ?'))act('/api/sleep?now=light','กำลังพักเครื่อง - กดปุ่ม BOOT เพื่อปลุก แล้วต่อ WiFi ใหม่');};
$('bDeep').onclick=()=>{if(confirm('นาฬิกาจะหลับ 60 วินาทีแล้วบูตใหม่ ค่าปกติและผลในหน่วยความจำจะหาย ดำเนินการต่อ?'))act('/api/sleep?now=deep&sec=60','นาฬิกาจะหลับ 60 วินาที แล้วต่อ WiFi ใหม่');};

// นาฬิกาไม่มีนาฬิกาเวลาจริงสำรองไฟ -> ส่งเวลาจากมือถือไปให้ (ใช้ประทับเวลาในไฟล์ CSV)
post('/api/time?epoch='+Math.floor(Date.now()/1000)).catch(()=>{});
load();paintQ();loadRes();loadMl();
(async function loop(){await live(true);setTimeout(loop,1000);})();
</script>
</body></html>)HTML";

// ---------------------------------------------------------------------
//  หน้าอัปเดตเฟิร์มแวร์ไร้สาย (OTA)  http://192.168.4.1/update  (ถามรหัส admin / polygraph-ota)
//  ส่งไฟล์ firmware.bin ด้วย XMLHttpRequest เพื่อแสดงความคืบหน้า — เบราว์เซอร์แนบรหัสผ่านที่กรอก
//  ตอนเปิดหน้านี้ไปให้เอง (Basic auth ของ origin เดียวกัน)
// ---------------------------------------------------------------------
static const char UPDATE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="th"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>อัปเดตเฟิร์มแวร์ - Wireless Polygraph</title>
<style>
body{margin:0;font:15px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans Thai",Tahoma,sans-serif;background:#eef1f6;color:#18212f}
header{background:#173f73;color:#fff;padding:12px 16px;font-weight:600}
main{max-width:560px;margin:16px auto;padding:0 12px}
.card{background:#fff;border:1px solid #d9dfe9;border-radius:10px;padding:16px}
ol{padding-left:20px;color:#5f6b80;font-size:14px}
input[type=file]{width:100%;margin:10px 0;font:inherit}
button{font:inherit;padding:10px 16px;border-radius:8px;border:1px solid #173f73;background:#173f73;color:#fff;min-height:44px;cursor:pointer}
button:disabled{opacity:.4}
.bar{height:10px;background:#e5e9f0;border-radius:5px;overflow:hidden;margin-top:12px}.bar i{display:block;height:100%;width:0;background:#173f73}
#msg{margin-top:10px;min-height:22px}.ok{color:#1a7f37}.bad{color:#b42318}a{color:#173f73}
</style></head><body>
<header>อัปเดตเฟิร์มแวร์ไร้สาย (OTA)</header>
<main><div class="card">
<ol><li>ใน VS Code กด Build แล้วหาไฟล์ <b>.pio/build/esp32c3/firmware.bin</b></li>
<li>เลือกไฟล์ด้านล่างแล้วกด "อัปโหลด" — ห้ามปิดหน้านี้หรือถอดแบตระหว่างอัปโหลด</li>
<li>เสร็จแล้วนาฬิการีสตาร์ทเอง ถ้าเฟิร์มแวร์ใหม่บูตไม่ผ่าน จะย้อนกลับเวอร์ชันเดิมให้อัตโนมัติ (rollback)</li></ol>
<input type="file" id="f" accept=".bin,application/octet-stream">
<button id="go" disabled>อัปโหลด</button>
<div class="bar"><i id="pb"></i></div><div id="msg"></div>
<p><a href="/">กลับหน้าหลัก</a></p>
</div></main>
<script>
const f=document.getElementById('f'),go=document.getElementById('go'),pb=document.getElementById('pb'),msg=document.getElementById('msg');
function say(t,c){msg.textContent=t;msg.className=c||'';}
f.onchange=()=>{go.disabled=!f.files.length;if(f.files.length)say('ไฟล์ '+f.files[0].name+' ('+Math.round(f.files[0].size/1024)+' KB)');};
go.onclick=()=>{
 const file=f.files[0];if(!file)return;
 if(!/\.bin$/i.test(file.name)){say('ต้องเป็นไฟล์ .bin','bad');return;}
 go.disabled=true;f.disabled=true;
 const fd=new FormData();fd.append('firmware',file,file.name);
 const x=new XMLHttpRequest();x.open('POST','/update');
 x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded*100/e.total);pb.style.width=p+'%';say('กำลังอัปโหลด '+p+'%');}};
 x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(e){}
  if(x.status===200&&j.ok){pb.style.width='100%';say('สำเร็จ นาฬิกากำลังรีสตาร์ท รอประมาณ 10 วินาทีแล้วต่อ WiFi ใหม่','ok');setTimeout(()=>{location.href='/'},12000);}
  else{say('อัปเดตไม่สำเร็จ: '+(j.error||('HTTP '+x.status)),'bad');go.disabled=false;f.disabled=false;}};
 x.onerror=()=>{say('การเชื่อมต่อขาดระหว่างอัปโหลด ลองใหม่อีกครั้ง','bad');go.disabled=false;f.disabled=false;};
 x.send(fd);
};
</script></body></html>)HTML";
