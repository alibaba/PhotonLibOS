import React from 'react';
import clsx from 'clsx';
import styles from './styles.module.css';
import Translate from '@docusaurus/Translate';

const FeatureList = [
  {
    title: <Translate>Carefully-selected C++ libraries</Translate>,
    Svg: require('@site/static/img/homepage/connection.svg').default,
    description: (
      <>
      <Translate>Help connect user apps and the OS.</Translate>
      </>
    ),
  },  
  {
    title: <Translate>High performance coroutine runtime</Translate>,
    Svg: require('@site/static/img/homepage/runtime.svg').default,
    description: (
      <>
      <Translate>Stackful coroutine. Symmetric scheduler. Non-blocking IO engine. Support io_uring.</Translate>
      </>
    ),
  },
  {
    title: <Translate>Multiple platforms and architectures</Translate>,
    Svg: require('@site/static/img/homepage/platforms.svg').default,
    description: (
      <>
      <Translate>Support Linux and macOS, on x86 and ARM.</Translate>
      </>
    ),
  },
  {
    title: <Translate>Well-designed assembly code</Translate>,
    Svg: require('@site/static/img/homepage/binary.svg').default,
    description: (
      <>
      <Translate>Reduce overhead on the critical path.</Translate>
      </>
    ),
  },
  {
    title: <Translate>Fully compatible API toward C++ std and POSIX</Translate>,
    Svg: require('@site/static/img/homepage/api.svg').default,
    description: (
      <>
      <Translate>Easy to learn. Less effort to integrate to a legacy codebase.</Translate>
      </>
    ),
  },
];

function Feature({Svg, title, description}) {
  return (
    <div className={clsx('col col--4')}>
      <div className="text--center">
        <Svg className={styles.featureSvg} role="img" />
      </div>
      <div className="text--center padding-horiz--md">
        <h3>{title}</h3>
        <p>{description}</p>
      </div>
    </div>
  );
}

export default function HomepageFeatures() {
  return (
    <section className={styles.features}>
      <div className="container">
        <div className="row">
          {FeatureList.map((props, idx) => (
            <Feature key={idx} {...props} />
          ))}
        </div>
      </div>
    </section>
  );
}
