const path = require("path");

/** @type {import("webpack").Configuration} */
module.exports = (env = {}) => {
  const prod = !!(
	env.prod ??
	env.production ??
	(env.mode ? env.mode === "prod" : false)
  );

  return {
	context: path.resolve(__dirname, "./src/"),
	mode: prod ? "production" : "development",
	target: "web",
	node: false,

	entry: {
	  alt1lite: "./main.ts",
	  "appframe/index": "./appframe/index.tsx",
	  "appframe/alt1api": "./appframe/alt1api.ts",
	  "overlayframe/index": "./overlayframe/index.tsx",
	  "settings/index": "./settings/index.tsx",
	  "tooltip/index": "./tooltip/index.tsx",
	  "tests/index": "./tests/index.ts",
	},

	output: {
	  path: path.resolve(__dirname, "dist"),
	  filename: "[name].bundle.js",
	  chunkFilename: prod ? "[name]_[chunkhash].bundle.js" : "[name].bundle.js",
	  globalObject: "(typeof self!='undefined'?self:this)",
	},

	resolve: {
	  extensions: [".wasm", ".tsx", ".ts", ".mjs", ".jsx", ".js", ".json"],
	},

	devtool: prod ? "source-map" : "eval-source-map",

	externals: [
	  { canvas: { root: "null", commonjs: null, commonjs2: null, amd: null } },
	  { sharp: { root: "null", commonjs: null, commonjs2: null, amd: null } },
	],

	module: {
	  parser: {
		javascript: { commonjsMagicComments: true },
	  },
	  rules: [
		{
		  test: /\.(ts|tsx)$/,
		  use: [{ loader: "ts-loader" }],
		},
		{
		  test: /\.css$/,
		  use: ["style-loader", "css-loader"],
		},
		{
		  test: /\.scss$/,
		  use: ["style-loader", "css-loader", "sass-loader"],
		},

		{
		  test: /\.data\.png$/i,
		  use: [{ loader: "alt1/imagedata-loader" }],
		},
		{
		  test: /\.(png|jpg|gif)$/i,
		  exclude: /\.data\.png$/i,
		  use: [
			{
			  loader: "url-loader",
			  options: {
				limit: 8192,
				esModule: false,
				name: "[path][name].[ext]",
			  },
			},
		  ],
		},

		{
		  test: /\.json$/,
		  type: "javascript/auto",
		  use: [{ loader: "json-loader" }],
		},
		{
		  test: /\.fontmeta\.json$/,
		  type: "javascript/auto",
		  use: [{ loader: "alt1/font-loader" }],
		},
		{
		  test: /\.html$/,
		  use: [
			{ loader: "file-loader", options: { name: "[path][name].[ext]" } },
		  ],
		},
	  ],
	},

	optimization: {
	  minimize: prod,
	  moduleIds: prod ? "natural" : "named",
	},
  };
};
