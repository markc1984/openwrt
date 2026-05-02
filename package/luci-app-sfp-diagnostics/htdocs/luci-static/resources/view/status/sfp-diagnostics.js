'use strict';
'require view';
'require rpc';
'require ui';

const callScan = rpc.declare({
	object: 'luci.sfp',
	method: 'scan',
	expect: {
		'': {
			ethtool: false,
			devices: []
		}
	}
});

function empty(value) {
	return value == null || value === '';
}

function dash(value) {
	return empty(value) ? E('em', {}, _('n/a')) : value;
}

function fixed(value, decimals, unit) {
	if (value == null)
		return E('em', {}, _('n/a'));

	return Number(value).toFixed(decimals) + (unit ? ' ' + unit : '');
}

function power(diag, prefix) {
	const mw = diag?.[prefix + '_power_mw'];
	const dbm = diag?.[prefix + '_power_dbm'];
	let values = [];

	if (mw != null)
		values.push(Number(mw).toFixed(4) + ' mW');

	if (dbm != null)
		values.push(Number(dbm).toFixed(2) + ' dBm');

	return values.length ? values.join(' / ') : E('em', {}, _('n/a'));
}

function compareIfname(a, b) {
	return (a.ifname || '').localeCompare(b.ifname || '', undefined, { numeric: true });
}

function boolState(value, yes, no) {
	if (value === true)
		return yes;
	if (value === false)
		return no;

	return E('em', {}, _('n/a'));
}

function alarmState(value) {
	return boolState(value, _('Active'), _('Inactive'));
}

function entryValues(dev, key) {
	const values = [];

	for (const entry of dev.entries || [])
		if (entry.key === key && entry.value && !values.includes(entry.value))
			values.push(entry.value);

	return values;
}

function renderList(values) {
	if (!values.length)
		return E('em', {}, _('n/a'));

	if (values.length === 1)
		return values[0];

	return E('ul', { 'style': 'margin: 0; padding-left: 1.4em' },
		values.map(value => E('li', {}, value)));
}

function renderInfoTable(rows) {
	return E('div', { 'class': 'table' }, rows.map(function(row) {
		return E('div', { 'class': 'tr' }, [
			E('div', {
				'class': 'td left',
				'style': 'font-weight: bold; width: 33%'
			}, row[0]),
			E('div', { 'class': 'td left' }, dash(row[1]))
		]);
	}));
}

function renderDevicePane(dev, active) {
	const id = dev.identity || {};
	const diag = dev.diagnostics || {};
	const status = dev.status || {};
	const transceiverTypes = entryValues(dev, 'transceiver_type');
	const children = [];
	const rows = [
		[ _('Module present'), boolState(status.module_present ?? dev.ok, _('Yes'), _('No')) ],
		[ _('Loss of Signal (LOS) alarm'), alarmState(status.los_alarm) ],
		[ _('Transmit Fault (TXFAULT) alarm'), alarmState(status.tx_fault_alarm) ],
		[ _('Link status'), boolState(status.link_up, _('Up'), _('Down')) ],
		[ _('Vendor'), id.vendor_name ],
		[ _('Vendor Part'), id.vendor_pn ],
		[ _('Transceiver Serial Number'), id.vendor_sn ],
		[ _('Transceiver Date Code'), id.date_code ],
		[ _('Transceiver type(s)'), renderList(transceiverTypes.length ? transceiverTypes : (id.transceiver_type ? [ id.transceiver_type ] : [])) ],
		[ _('TX optical power'), power(diag, 'tx') ],
		[ _('RX optical power'), power(diag, 'rx') ],
		[ _('Bias Current'), fixed(diag.tx_bias_ma, 3, 'mA') ],
		[ _('Module Temperature'), fixed(diag.temperature_c, 2, 'C') ],
		[ _('Module Voltage'), fixed(diag.voltage_v, 4, 'V') ]
	];

	if (!dev.ok)
		children.push(E('div', { 'class': 'alert-message warning' },
			dev.error || _('SFP diagnostics unavailable.')));

	children.push(renderInfoTable(rows));

	if (dev.raw)
		children.push(E('details', { 'style': 'margin-top: 1em' }, [
			E('summary', {}, _('Raw ethtool output')),
			E('pre', { 'style': 'white-space: pre-wrap' }, dev.raw || dev.error || '')
		]));

	return E('div', {
		'data-tab': dev.ifname || 'unknown',
		'data-tab-title': dev.ifname || _('Unknown'),
		'data-tab-active': active ? 'true' : 'false'
	}, children);
}

return view.extend({
	load() {
		return L.resolveDefault(callScan(), { ethtool: false, devices: [] });
	},

	handleRefresh() {
		ui.showModal(_('Refreshing'), [
			E('p', { 'class': 'spinning' }, _('Collecting SFP diagnostics...'))
		]);

		location.reload();
	},

	render(data) {
		const devices = Array.isArray(data?.devices) ? data.devices.sort(compareIfname) : [];
		const available = devices.filter(dev => dev.ok);
		const body = [
			E('h2', {}, _('SFP/Optical Transceivers')),
			E('div', { 'class': 'cbi-map-descr' },
				_('Decoded pluggable module EEPROM and DOM/DDM diagnostics reported by ethtool.')),
			E('p', {}, E('button', {
				'class': 'btn cbi-button-action',
				'click': ui.createHandlerFn(this, 'handleRefresh')
			}, _('Refresh')))
		];

		if (!data?.ethtool) {
			body.push(E('div', { 'class': 'alert-message warning' },
				_('ethtool-full is not installed or ethtool is not executable.')));
			return body;
		}

		if (!devices.length) {
			body.push(E('div', { 'class': 'alert-message' },
				_('No SFP interfaces were available for diagnostics.')));
			return body;
		}

		if (!available.length)
			body.push(E('div', { 'class': 'alert-message warning' },
				_('No readable SFP module EEPROM data was reported.')));

		const tabGroup = E('div', {}, devices.map(function(dev, index) {
			return renderDevicePane(dev, index === 0);
		}));
		const viewNode = E('div', {}, body.concat([ tabGroup ]));

		ui.tabs.initTabGroup(tabGroup.childNodes);

		return viewNode;
	}
});
